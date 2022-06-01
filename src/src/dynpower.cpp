#if defined(TARGET_TX)

#include <dynpower.h>
#include <common.h>

// LQ-based boost defines
#define DYNPOWER_LQ_BOOST_THRESH_DIFF 20  // If LQ is dropped suddenly for this amount (relative), immediately boost to the max power configured.
#define DYNPOWER_LQ_BOOST_THRESH_MIN  50  // If LQ is below this value (absolute), immediately boost to the max power configured.
#define DYNPOWER_LQ_MOVING_AVG_K      8   // Number of previous values for calculating moving average. Best with power of 2.

// RSSI-based increment defines
#define DYNPOWER_RSSI_CNT 5               // Number of RSSI readings to average (straight average) to make an RSSI-based adjustment
#define DYNPOWER_RSSI_THRESH_UP 15        // RSSI < (Sensitivity+Up) -> raise power
#define DYNPOWER_RSSI_THRESH_DN 21        // RSSI > (Sensitivity+Dn) >- lower power

// SNR-based increment defines
#define DYNPOWER_LQ_THRESH_DN 95          // Min LQ for lowering power using SNR-based power lowering

template<uint8_t K, uint8_t SHIFT>
class MovingAvg
{
public:
  void init(uint32_t v) { _shiftedVal = v << SHIFT; };
  void add(uint32_t v) {  _shiftedVal = ((K - 1) * _shiftedVal + (v << SHIFT)) / K; };
  uint32_t getValue() const { return _shiftedVal >> SHIFT; };

  void operator=(const uint32_t &v) { init(v); };
  operator uint32_t () const { return getValue(); };
private:
  uint32_t _shiftedVal;
};

static MovingAvg<DYNPOWER_LQ_MOVING_AVG_K, 16> dynamic_power_mavg_lq;
static MeanAccumulator<int32_t, int8_t, -128> dynamic_power_mean_rssi;
static DynamicPowerTelemetryUpdate_e dynamic_power_updated;

extern bool IsArmed();
extern volatile uint32_t LastTLMpacketRecvMillis;

static void DynamicPower_SetToConfigPower()
{
    POWERMGNT::setPower((PowerLevels_e)config.GetPower());
}

void DynamicPower_Init()
{
    dynamic_power_mavg_lq = 100;
    dynamic_power_updated = dptuNoUpdate;
}

void ICACHE_RAM_ATTR DynamicPower_TelemetryUpdate(DynamicPowerTelemetryUpdate_e dptu)
{
    dynamic_power_updated = dptu;
}

void DynamicPower_Update()
{
  bool newTlmAvail = dynamic_power_updated == dptuNewLinkstats;
  bool lastTlmMissed = dynamic_power_updated == dptuMissed;
  dynamic_power_updated = dptuNoUpdate;

  int8_t rssi = (CRSF::LinkStatistics.active_antenna == 0) ? CRSF::LinkStatistics.uplink_RSSI_1 : CRSF::LinkStatistics.uplink_RSSI_2;

  // power is too strong and saturate the RX LNA
  if (newTlmAvail && (rssi >= -5))
  {
    DBGVLN("-power (overload)");
    POWERMGNT::decPower();
  }

  // When not using dynamic power, return here
  if (!config.GetDynamicPower())
  {
    // if RSSI is dropped enough, inc power back to the configured power
    if (newTlmAvail && (rssi <= -20))
    {
      DynamicPower_SetToConfigPower();
    }
    return;
  }

  //
  // The rest of the codes should be executeded only if dynamic power config is enabled
  //

  // =============  DYNAMIC_POWER_BOOST: Switch-triggered power boost up ==============
  // Or if telemetry is lost while armed (done up here because dynamic_power_updated is only updated on telemetry)
  uint8_t boostChannel = config.GetBoostChannel();
  bool armed = IsArmed();
  if ((connectionState == disconnected && armed) ||
    (boostChannel && (CRSF_to_BIT(CRSF::ChannelDataIn[AUX9 + boostChannel - 1]) == 0)))
  {
    DynamicPower_SetToConfigPower();
    return;
  }

  // How much available power is left for incremental increases
  uint8_t powerHeadroom = (uint8_t)config.GetPower() - (uint8_t)POWERMGNT::currPower();

  if (lastTlmMissed)
  {
    // If armed and missing telemetry, raise the power, but only after the first LinkStats is missed (which come
    // at most every 512ms). This delays the first increase, then will bump it once for each missed TLM after that
    // state == connected is not used: unplugging an RX will be connected and will boost power to max before disconnect
    if (armed && (powerHeadroom > 0))
    {
      uint32_t linkstatsInterval = 2U * TLMratioEnumToValue(config.GetTlm()) * ExpressLRS_currAirRate_Modparams->interval / 1000U;
      linkstatsInterval = std::max(linkstatsInterval, (uint32_t)512U);
      // Capture the last before now so it will always be <= now
      const uint32_t lastTlmMillis = LastTLMpacketRecvMillis;
      const uint32_t now = millis();
      if (now - lastTlmMillis > (linkstatsInterval + 2U))
      {
        DBGLN("+power (tlm)");
        POWERMGNT::incPower();
      }
    }
    return;
  }

  // If no new telemetry, no new LQ/SNR/RSSI to use for adjustment
  if (!newTlmAvail)
    return;

  // =============  LQ-based power boost up ==============
  // Quick boost up of power when detected any emergency LQ drops.
  // It should be useful for bando or sudden lost of LoS cases.
  uint32_t lq_current = CRSF::LinkStatistics.uplink_Link_quality;
  uint32_t lq_avg = dynamic_power_mavg_lq;
  int32_t lq_diff = lq_avg - lq_current;
  dynamic_power_mavg_lq.add(lq_current);
  // if LQ drops quickly (DYNPOWER_LQ_BOOST_THRESH_DIFF) or critically low below DYNPOWER_LQ_BOOST_THRESH_MIN, immediately boost to the configured max power.
  if (lq_diff >= DYNPOWER_LQ_BOOST_THRESH_DIFF || lq_current <= DYNPOWER_LQ_BOOST_THRESH_MIN)
  {
      DynamicPower_SetToConfigPower();
      return;
  }

  if (ExpressLRS_currAirRate_RFperfParams->DynpowerSnrThreshUp == DYNPOWER_SNR_THRESH_NONE)
  {
    // =============  RSSI-based power increment ==============
    // a simple threshold compared against N sample average of
    // rssi vs the sensitivity limit +/- some thresholds
    dynamic_power_mean_rssi.add(rssi);

    if (dynamic_power_mean_rssi.getCount() >= DYNPOWER_RSSI_CNT)
    {
      int32_t expected_RXsensitivity = ExpressLRS_currAirRate_RFperfParams->RXsensitivity;
      int8_t rssi_inc_threshold = expected_RXsensitivity + DYNPOWER_RSSI_THRESH_UP;
      int8_t rssi_dec_threshold = expected_RXsensitivity + DYNPOWER_RSSI_THRESH_DN;
      int8_t avg_rssi = dynamic_power_mean_rssi.mean(); // resets it too
      if ((avg_rssi < rssi_inc_threshold) && (powerHeadroom > 0))
      {
        DBGLN("+power (rssi)");
        POWERMGNT::incPower();
      }
      else if (avg_rssi > rssi_dec_threshold && lq_avg >= DYNPOWER_LQ_THRESH_DN)
      {
        DBGVLN("-power (rssi)");
        POWERMGNT::decPower();
      }
    }
  } // ^^ if RSSI-based
  else
  {
    // =============  SNR-based power increment ==============
    // Decrease the power if SNR above threshold and LQ is good
    // Increase the power for each (X) SNR below the threshold
    int8_t snr = CRSF::LinkStatistics.uplink_SNR;
    if (snr >= ExpressLRS_currAirRate_RFperfParams->DynpowerSnrThreshDn && lq_avg >= DYNPOWER_LQ_THRESH_DN)
    {
      DBGVLN("-power (snr)");
      POWERMGNT::decPower();
    }

    while ((snr <= ExpressLRS_currAirRate_RFperfParams->DynpowerSnrThreshUp) && (powerHeadroom > 0))
    {
      DBGLN("+power (snr)");
      POWERMGNT::incPower();
      // Every power doubling will theoretically increase the SNR by 3dB, but closer to 2dB in testing
      snr += 2*4;
      --powerHeadroom;
    }
  } // ^^ if SNR-based
}

#endif // TARGET_TX