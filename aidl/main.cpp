/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#define LOG_TAG "android.hardware.health-service.qti"

#include <android-base/logging.h>

#include <algorithm>
#include <android/binder_interface_utils.h>
#include <cutils/klog.h>
#include <cutils/properties.h>
#include <health/utils.h>
#include <health-impl/ChargerUtils.h>
#include <health-impl/Health.h>

#define ARRAY_SIZE(x)     (sizeof(x) / sizeof((x)[0]))

typedef enum soc_id {
        MSM_NEO_LA = 554,
        MSM_NEO_LE = 525,
        MSM_NEO_LA_V2 = 579,
        MSM_SERAPH = 673,
        MSM_SERAPHP = 672,
}soc_id_t;

static const enum soc_id target_no_psy[] = {
        MSM_NEO_LA,
        MSM_NEO_LE,
        MSM_NEO_LA_V2,
        MSM_SERAPH,
        MSM_SERAPHP,
};

using aidl::android::hardware::health::HalHealthLoop;
using aidl::android::hardware::health::Health;

#if !CHARGER_FORCE_NO_UI
using aidl::android::hardware::health::charger::ChargerCallback;
using aidl::android::hardware::health::charger::ChargerModeMain;
namespace aidl::android::hardware::health {
class ChargerCallbackImpl : public ChargerCallback {
  public:
    ChargerCallbackImpl(const std::shared_ptr<Health>& service) : ChargerCallback(service) {}
    bool ChargerEnableSuspend() override { return true; }
};
} //namespace aidl::android::hardware::health
#endif

static constexpr const char* gInstanceName = "default";
static constexpr std::string_view gChargerArg{"--charger"};

static constexpr std::array<std::string_view, 3> ucsiPSYNames = {
        "ucsi-source-psy-soc:qcom,pmic_glink:qcom,ucsi1",
        "ucsi-source-psy-soc:qcom,pmic_glink:qcom,ucsi2",
        "ucsi-source-psy-soc:pmic-glink:ucsi-glink1"
};

#define RETRY_COUNT    100

void qti_healthd_board_init(struct healthd_config *hc)
{
    FILE *fp = NULL;
    int fd;
    unsigned char retries = RETRY_COUNT;
    int ret = 0;
    unsigned char buf;
    char prop_str[PROPERTY_VALUE_MAX];
    int soc_id_prop = 0;
    bool is_no_batt_psy;
    int soc = property_get_int32("ro.vendor.qti.soc_id", -1);

    hc->ignorePowerSupplyNames.push_back(android::String8(ucsiPSYNames[0]));
    hc->ignorePowerSupplyNames.push_back(android::String8(ucsiPSYNames[1]));
    hc->ignorePowerSupplyNames.push_back(android::String8(ucsiPSYNames[2]));

    is_no_batt_psy = property_get_bool("persist.vendor.hal_health.no_batt_psy", false);

    if (soc <= 0 && (fp = fopen("/sys/devices/soc0/soc_id", "r")) != NULL) {
        fscanf(fp, "%u", &soc);
        fclose(fp);
    }
    soc_id_prop = soc;

    if (!is_no_batt_psy) {
        for (int idx = 0; idx < ARRAY_SIZE(target_no_psy); idx++) {
             if(soc_id_prop == target_no_psy[idx]) {
                KLOG_INFO(LOG_TAG, "no support for batt_psy with socid:%d \n",soc_id_prop);
                return;
           }
        }
    } else {
        KLOG_INFO(LOG_TAG, "no support for batt_psy\n");
        return;
    }

retry:
    if (!retries) {
        KLOG_ERROR(LOG_TAG, "Cannot open battery/capacity, fd=%d\n", fd);
        return;
    }

    fd = open("/sys/class/power_supply/battery/capacity", 0440);
    if (fd >= 0) {
        KLOG_INFO(LOG_TAG, "opened battery/capacity after %d retries\n", RETRY_COUNT - retries);
        while (retries) {
            ret = read(fd, &buf, 1);
            if(ret >= 0) {
                KLOG_INFO(LOG_TAG, "Read Batt Capacity after %d retries ret : %d\n", RETRY_COUNT - retries, ret);
                close(fd);
                return;
            }

            retries--;
            usleep(100000);
        }

        KLOG_ERROR(LOG_TAG, "Failed to read Battery Capacity ret=%d\n", ret);
        close(fd);
        return;
    }

    retries--;
    usleep(100000);
    goto retry;
}

namespace aidl::android::hardware::health {

// State of health for fuel gauges without a state_of_health sysfs node: learned full-charge
// capacity over design capacity, in percent (BatteryManager.BATTERY_PROPERTY_STATE_OF_HEALTH,
// Settings > Battery > Battery information).
static int32_t StateOfHealth(int64_t full_uah, int64_t design_uah) {
    if (full_uah <= 0 || design_uah <= 0) return 0;
    int64_t soh = (full_uah * 100 + design_uah / 2) / design_uah;
    return static_cast<int32_t>(std::clamp<int64_t>(soh, 1, 100));
}

class QtiHealth : public Health {
  public:
    using Health::Health;

    ndk::ScopedAStatus getBatteryHealthData(BatteryHealthData* out) override {
        auto status = Health::getBatteryHealthData(out);
        if (status.isOk() && out->batteryStateOfHealth <= 0) {
            out->batteryStateOfHealth =
                    StateOfHealth(getFullChargeUah(), getFullChargeDesignCapacityUah());
        }
        return status;
    }

  protected:
    void UpdateHealthInfo(HealthInfo* health_info) override {
        Health::UpdateHealthInfo(health_info);
        if (health_info->batteryHealthData.has_value() &&
            health_info->batteryHealthData->batteryStateOfHealth <= 0) {
            health_info->batteryHealthData->batteryStateOfHealth =
                    StateOfHealth(health_info->batteryFullChargeUah,
                                  health_info->batteryFullChargeDesignCapacityUah);
        }
    }
};

}  // namespace aidl::android::hardware::health

int main(int argc, char** argv) {
#ifdef __ANDROID_RECOVERY__
    android::base::InitLogging(argv, android::base::KernelLogger);
#endif
    auto config = std::make_unique<healthd_config>();
    ::android::hardware::health::InitHealthdConfig(config.get());
    qti_healthd_board_init(config.get());
    auto binder = ndk::SharedRefBase::make<aidl::android::hardware::health::QtiHealth>(
            gInstanceName, std::move(config));

    if (argc >= 2 && argv[1] == gChargerArg) {
#if !CHARGER_FORCE_NO_UI
        KLOG_INFO(LOG_TAG, "Starting charger mode with UI.");
        auto charger_callback = std::make_shared<aidl::android::hardware::health::ChargerCallbackImpl>(binder);
        return ChargerModeMain(binder, charger_callback);
#endif
        KLOG_INFO(LOG_TAG, "Starting charger mode without UI.");
    } else {
        KLOG_INFO(LOG_TAG, "Starting health HAL.");
    }

    auto hal_health_loop = std::make_shared<HalHealthLoop>(binder, binder);
    return hal_health_loop->StartLoop();
}
