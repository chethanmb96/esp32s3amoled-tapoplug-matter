// =============================================================================
// tapo-matter-display.cpp — Phase 2: Matter Controller Discovery
//
// Target : LILYGO T-Display-S3 AMOLED / ESP32-S3
// Purpose: Commission the Tapo P116M and dump all Matter attributes to serial.
//
// Build  : ./build.sh build
// Config : ./build.sh menuconfig  →  "Tapo P116M Configuration"
// Flash  : ./build.sh flash monitor
//
// Wi-Fi note:
//   esp_matter::start() internally calls InitWiFiStack() which runs
//   esp_netif_init(), esp_netif_create_default_wifi_sta(), esp_wifi_init().
//   We must NOT duplicate those calls. We only call:
//     esp_wifi_set_mode / set_config / start / connect
//   after esp_matter::start() returns.
// =============================================================================

#include "sdkconfig.h"

// ── ESP-IDF ──────────────────────────────────────────────────────────────────
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

// ── ESP-Matter ────────────────────────────────────────────────────────────────
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_matter_controller_read_command.h>
#include <esp_matter_controller_subscribe_command.h>
#include <esp_matter_controller_utils.h>

// ── CHIP / Matter SDK ─────────────────────────────────────────────────────────
#include <app/ConcreteAttributePath.h>
#include <app/MessageDef/StatusIB.h>
#include <lib/core/TLVReader.h>
#include <lib/core/DataModelTypes.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/PlatformManager.h>
#include <lib/support/TypeTraits.h>
#include <credentials/attestation_verifier/DefaultDeviceAttestationVerifier.h>
#include <credentials/attestation_verifier/DeviceAttestationVerifier.h>
#include <esp_matter_attestation_trust_store.h>

class PermissiveDeviceAttestationVerifier : public chip::Credentials::DefaultDACVerifier
{
public:
    PermissiveDeviceAttestationVerifier(const chip::Credentials::AttestationTrustStore * paaRootStore)
        : chip::Credentials::DefaultDACVerifier(paaRootStore) {}

    void VerifyAttestationInformation(const chip::Credentials::DeviceAttestationVerifier::AttestationInfo & info,
                                      chip::Callback::Callback<chip::Credentials::DeviceAttestationVerifier::OnAttestationInformationVerification> * onCompletion) override
    {
        ESP_LOGI("tapo-discovery", "Device Attestation: accepting commercial DAC from vendor 0x%04X, product 0x%04X",
                 info.vendorId, info.productId);
        if (onCompletion) {
            onCompletion->mCall(onCompletion->mContext, info, chip::Credentials::AttestationVerificationResult::kSuccess);
        }
    }
};

static PermissiveDeviceAttestationVerifier s_permissive_verifier(chip::Credentials::get_attestation_trust_store());

static const char *TAG = "tapo-discovery";

// ─────────────────────────────────────────────────────────────────────────────
// Wi-Fi helpers
// Note: esp_netif and esp_wifi are already initialised by InitWiFiStack()
// inside esp_matter::start(). We only configure and connect here.
// ─────────────────────────────────────────────────────────────────────────────
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
static int s_retry_num = 0;
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if ((event_base == IP_EVENT && (event_id == IP_EVENT_STA_GOT_IP || event_id == IP_EVENT_GOT_IP6)) ||
        (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Called AFTER esp_matter::start() so that InitWiFiStack() has already run.
static esp_err_t wifi_connect(void)
{
    s_wifi_event_group = xEventGroupCreate();

    esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_GOT_IP6, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &wifi_event_handler, NULL, NULL);

    // Check if IP is already acquired
    esp_netif_ip_info_t ip_info = {};
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        ESP_LOGI(TAG, "Wi-Fi already connected — IP: " IPSTR, IP2STR(&ip_info.ip));
        return ESP_OK;
    }

    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid,     CONFIG_TAPO_WIFI_SSID,     sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_TAPO_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    ESP_LOGI(TAG, "Waiting for Wi-Fi IP on SSID: %s ...", CONFIG_TAPO_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        ESP_LOGI(TAG, "Wi-Fi confirmed connected — IP: " IPSTR, IP2STR(&ip_info.ip));
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Wi-Fi connection failed");
    return ESP_FAIL;
}

// ─────────────────────────────────────────────────────────────────────────────
// Matter event callback
// ─────────────────────────────────────────────────────────────────────────────
static void app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg)
{
    (void)arg;
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kWiFiConnectivityChange:
        ESP_LOGI(TAG, "Matter: WiFi connectivity change");
        break;
    case chip::DeviceLayer::DeviceEventType::kInternetConnectivityChange:
        ESP_LOGI(TAG, "Matter: Internet connectivity change");
        break;
    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Pairing callbacks
// ─────────────────────────────────────────────────────────────────────────────
static volatile bool s_commissioned = false;
static volatile bool s_commission_failed = false;

static void on_pase(CHIP_ERROR err)
{
    if (err == CHIP_NO_ERROR) {
        ESP_LOGI(TAG, "PASE session established with P116M ✓");
    } else {
        ESP_LOGE(TAG, "PASE failed: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

static void on_commissioning_success(chip::ScopedNodeId peer_id)
{
    ESP_LOGI(TAG, "✓ Commissioned P116M! NodeId=0x%016llX FabricIndex=%u",
             (unsigned long long)peer_id.GetNodeId(),
             (unsigned)peer_id.GetFabricIndex());
    s_commissioned = true;
}

static void on_commissioning_failure(chip::ScopedNodeId peer_id, CHIP_ERROR error,
                                     chip::Controller::CommissioningStage stage,
                                     std::optional<chip::Credentials::AttestationVerificationResult> extra)
{
    ESP_LOGE(TAG, "✗ Commissioning FAILED at stage %d: %" CHIP_ERROR_FORMAT,
             (int)stage, error.Format());
    s_commission_failed = true;
}

// ── AMOLED Display & UI ───────────────────────────────────────────────────────
#include "display_ui.h"
#include "rm67162.h"

static ui_state_t g_ui_state = {
    .power_w = 0.0f,
    .voltage_v = 230.0f,
    .current_a = 0.0f,
    .energy_kwh = 0.0f,
    .is_on = false,
    .is_connected = false,
    .status_msg = "CONNECTING..."
};

static float s_accumulated_energy_kwh = 0.0f;
static float s_plug_energy_kwh = 0.0f;
static bool s_plug_energy_reported = false;

// ─────────────────────────────────────────────────────────────────────────────
// Attribute report callback — updates live UI telemetry state
// ─────────────────────────────────────────────────────────────────────────────

// Robustly extract any TLV integer (signed/unsigned, 8/16/32/64-bit) as int64_t.
// Returns false only if data is null or truly not an integer type.
static bool tlv_get_int64(chip::TLV::TLVReader *data, int64_t &out)
{
    if (!data) return false;
    chip::TLV::TLVType t = data->GetType();
    switch (t) {
    case chip::TLV::kTLVType_Boolean:   { bool b = false;     if (data->Get(b)     == CHIP_NO_ERROR) { out = b ? 1 : 0;   return true; } break; }
    case chip::TLV::kTLVType_UnsignedInteger: {
        uint64_t u = 0;
        if (data->Get(u) == CHIP_NO_ERROR) { out = (int64_t)u; return true; }
        break;
    }
    case chip::TLV::kTLVType_SignedInteger: {
        int64_t s = 0;
        if (data->Get(s) == CHIP_NO_ERROR) { out = s; return true; }
        break;
    }
    default: break;
    }
    return false;
}

static void attr_report_cb(uint64_t node_id,
                            const chip::app::ConcreteDataAttributePath &path,
                            chip::TLV::TLVReader *data,
                            const chip::app::StatusIB &status)
{
    if (status.mStatus != chip::Protocols::InteractionModel::Status::Success || data == nullptr) {
        return;
    }

    g_ui_state.is_connected = true;
    g_ui_state.status_msg = "TAPO P116M";

    int64_t raw_val = 0;

    // 1. Cluster 0x0006: OnOff (always boolean)
    if (path.mEndpointId == 1 && path.mClusterId == 0x0006 && path.mAttributeId == 0x0000) {
        bool val = false;
        if (data->Get(val) == CHIP_NO_ERROR) {
            g_ui_state.is_on = val;
        }
        goto done;
    }

    // 2. Cluster 0x0090: Electrical Power Measurement (Matter 1.3+)
    if (path.mEndpointId == 1 && path.mClusterId == 0x0090) {
        if (!tlv_get_int64(data, raw_val)) goto done;
        switch (path.mAttributeId) {
        case 0x0008: // Active Power (mW)
        case 0x000D: // RMS Power (mW)
            g_ui_state.power_w = (raw_val > 0) ? ((float)raw_val / 1000.0f) : 0.0f;
            break;
        case 0x000B: // RMS Voltage (mV)
        case 0x0004:
            if (raw_val > 0) g_ui_state.voltage_v = (float)raw_val / 1000.0f;
            break;
        case 0x000C: // RMS Current (mA)
        case 0x0005:
            g_ui_state.current_a = (raw_val > 0) ? ((float)raw_val / 1000.0f) : 0.0f;
            break;
        default: goto done;
        }
        goto done;
    }

    // 3. Cluster 0x0091: Electrical Energy Measurement (Matter 1.3+) / 0x0702 Simple Metering
    if (path.mEndpointId == 1 && (path.mClusterId == 0x0091 || path.mClusterId == 0x0702)) {
        if (tlv_get_int64(data, raw_val) && raw_val >= 0) {
            // raw_val could be in mWh, Wh, or pulse units
            if (raw_val > 1000000) {
                s_plug_energy_kwh = (float)raw_val / 1000000.0f; // from mWh -> kWh
            } else if (raw_val > 1000) {
                s_plug_energy_kwh = (float)raw_val / 1000.0f;    // from Wh -> kWh
            } else {
                s_plug_energy_kwh = (float)raw_val;
            }
            s_plug_energy_reported = true;
            g_ui_state.energy_kwh = s_plug_energy_kwh;
        }
        goto done;
    }

    // 4. Cluster 0x0B04: Electrical Measurement (legacy ZCL)
    if (path.mEndpointId == 1 && path.mClusterId == 0x0B04) {
        if (!tlv_get_int64(data, raw_val)) goto done;
        switch (path.mAttributeId) {
        case 0x050B: // Active Power (W)
            if (raw_val >= 0) {
                float w = (raw_val > 100000) ? ((float)raw_val / 1000.0f) : ((float)raw_val / 10.0f);
                g_ui_state.power_w = w;
            }
            break;
        case 0x0505: // RMS Voltage (V * 10)
            if (raw_val > 0) g_ui_state.voltage_v = (float)raw_val / 10.0f;
            break;
        case 0x0508: // RMS Current (mA)
            g_ui_state.current_a = (raw_val > 0) ? ((float)raw_val / 1000.0f) : 0.0f;
            break;
        default: goto done;
        }
        goto done;
    }

done:
    // Refresh AMOLED Display immediately on attribute arrival
    display_ui_update(&g_ui_state);
}

static volatile bool s_read_done = false;

static void read_done_cb(uint64_t node_id,
                         const chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> &,
                         const chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> &)
{
    s_read_done = true;
}

static void on_connect_fail(void *context, const chip::ScopedNodeId &peerId, CHIP_ERROR error)
{
    ESP_LOGW(TAG, "Connection to node 0x%016llX failed: %" CHIP_ERROR_FORMAT,
             (unsigned long long)peerId.GetNodeId(), error.Format());
}

static void on_read_error(uint64_t node_id, CHIP_ERROR error)
{
    ESP_LOGW(TAG, "Read from node 0x%016llX error: %" CHIP_ERROR_FORMAT,
             (unsigned long long)node_id, error.Format());
}

// ─────────────────────────────────────────────────────────────────────────────
// Subscription management — restart on termination
// ─────────────────────────────────────────────────────────────────────────────
static volatile bool s_subscription_active = false;
static volatile bool s_subscription_pending = false; // send_command issued, waiting for established cb

static void on_subscription_established(uint64_t node_id, uint32_t sub_id)
{
    ESP_LOGI(TAG, "Subscription established with P116M (node=0x%llX, sub_id=%u)",
             (unsigned long long)node_id, (unsigned)sub_id);
    s_subscription_pending = false;
    s_subscription_active  = true;
}

static void on_subscription_terminated(uint64_t node_id, uint32_t sub_id)
{
    ESP_LOGW(TAG, "Subscription to P116M TERMINATED (node=0x%llX, sub_id=%u) — will reconnect.",
             (unsigned long long)node_id, (unsigned)sub_id);
    s_subscription_active  = false;
    s_subscription_pending = false;
    // Mark connection as lost on display
    g_ui_state.is_connected = false;
    g_ui_state.status_msg = "RECONNECTING...";
    display_ui_update(&g_ui_state);
}

// Start one subscribe_command for the given clusters.
// Returns true on success.
static bool start_subscription(uint64_t node_id)
{
    using namespace chip::app;
    const int kNumPaths = 6;
    chip::Platform::ScopedMemoryBufferWithSize<AttributePathParams> attr_paths;
    attr_paths.Alloc(kNumPaths);
    if (!attr_paths.Get()) {
        ESP_LOGE(TAG, "Failed to alloc attribute paths");
        return false;
    }

    // OnOff — relay state
    attr_paths[0] = AttributePathParams(/*ep*/1, /*cluster*/0x0006, /*attr*/0x0000);
    // EPM: Active Power (mW)
    attr_paths[1] = AttributePathParams(/*ep*/1, /*cluster*/0x0090, /*attr*/0x0008);
    // EPM: RMS Voltage (mV)
    attr_paths[2] = AttributePathParams(/*ep*/1, /*cluster*/0x0090, /*attr*/0x000B);
    // EPM: RMS Current (mA)
    attr_paths[3] = AttributePathParams(/*ep*/1, /*cluster*/0x0090, /*attr*/0x000C);
    // EEM: Cumulative Energy (Cluster 0x0091:0x0000)
    attr_paths[4] = AttributePathParams(/*ep*/1, /*cluster*/0x0091, /*attr*/0x0000);
    // Legacy ZCL fallback: Active Power (Cluster 0x0B04)
    attr_paths[5] = AttributePathParams(/*ep*/1, /*cluster*/0x0B04, /*attr*/0x050B);

    chip::Platform::ScopedMemoryBufferWithSize<EventPathParams> event_paths;

    auto *sub_cmd = chip::Platform::New<esp_matter::controller::subscribe_command>(
        node_id,
        std::move(attr_paths),
        std::move(event_paths),
        /* min_interval */ static_cast<uint16_t>(0),
        /* max_interval */ static_cast<uint16_t>(5),
        /* auto_resubscribe */ true,
        attr_report_cb,
        /* event_cb */ nullptr,
        on_subscription_established,
        on_subscription_terminated,
        on_connect_fail,
        /* keep_subscription */ true
    );

    if (!sub_cmd) {
        ESP_LOGE(TAG, "Failed to allocate subscribe_command");
        return false;
    }

    chip::DeviceLayer::PlatformMgr().LockChipStack();
    esp_err_t err = sub_cmd->send_command();
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "subscribe send_command returned: %s — will retry", esp_err_to_name(err));
        chip::Platform::Delete(sub_cmd);
        return false;
    }

    ESP_LOGI(TAG, "Subscription command sent to P116M (min=0s, max=5s, explicit 6 paths)");
    s_subscription_pending = true;   // Block re-entry until established cb fires
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Telemetry loop — subscribes to real-time attribute push stream from Tapo P116M
// ─────────────────────────────────────────────────────────────────────────────
static void telemetry_task(void *pvParam)
{
    const uint64_t node_id = CONFIG_TAPO_MATTER_NODE_ID;

    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, " LIVE TELEMETRY TASK STARTED (Node 0x%016llX)", (unsigned long long)node_id);
    ESP_LOGI(TAG, "=================================================");

    // Initial render: show dashboard in connecting state
    g_ui_state.status_msg = "TAPO P116M";
    g_ui_state.voltage_v = 230.0f;
    display_ui_update(&g_ui_state);

    // Give Matter DNS-SD discovery a moment to resolve the node
    vTaskDelay(pdMS_TO_TICKS(3000));

    uint64_t last_energy_calc_time_us = esp_timer_get_time();
    uint32_t retry_backoff_ms = 5000;

    while (1) {
        // Integrate continuous energy consumption (Riemann sum Wh/kWh)
        uint64_t now_us = esp_timer_get_time();
        float dt_hours = (float)(now_us - last_energy_calc_time_us) / 3600000000.0f;
        last_energy_calc_time_us = now_us;

        if (g_ui_state.is_on && g_ui_state.power_w > 0.0f) {
            s_accumulated_energy_kwh += (g_ui_state.power_w / 1000.0f) * dt_hours;
        }

        if (!s_plug_energy_reported) {
            g_ui_state.energy_kwh = s_accumulated_energy_kwh;
        }

        if (!s_subscription_active && !s_subscription_pending) {
            ESP_LOGI(TAG, "Attempting subscription to P116M (backoff %lu ms)...", (unsigned long)retry_backoff_ms);
            bool ok = start_subscription(node_id);
            if (ok) {
                for (int w = 0; w < 150 && s_subscription_pending && !s_subscription_active; w++) {
                    display_ui_update(&g_ui_state);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                if (s_subscription_active) {
                    retry_backoff_ms = 5000;
                } else {
                    s_subscription_pending = false;
                    retry_backoff_ms = (retry_backoff_ms < 30000) ? retry_backoff_ms * 2 : 30000;
                    g_ui_state.is_connected = false;
                    g_ui_state.status_msg = "RECONNECTING...";
                    ESP_LOGW(TAG, "Subscription did not establish within 15s — retry in %lu ms", (unsigned long)retry_backoff_ms);
                }
            } else {
                retry_backoff_ms = (retry_backoff_ms < 30000) ? retry_backoff_ms * 2 : 30000;
                g_ui_state.is_connected = false;
                g_ui_state.status_msg = "RECONNECTING...";
                uint32_t slept = 0;
                while (slept < retry_backoff_ms) {
                    display_ui_update(&g_ui_state);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    slept += 200;
                }
                continue;
            }
        }

        // Redraw UI at 10Hz
        display_ui_update(&g_ui_state);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// app_main
// ─────────────────────────────────────────────────────────────────────────────
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  Tapo P116M Matter Display Controller     ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════╝");

    // 0. Initialize AMOLED Display & Show Dashboard
    display_ui_init();
    display_ui_update(&g_ui_state);

    // 1. NVS — required by Matter for fabric/credential storage
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS: wiping partition (no free pages or version mismatch)");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS: initialized");

    // 2. Start Matter stack
    ESP_LOGI(TAG, "Starting Matter stack (controller-only mode)...");
    ESP_ERROR_CHECK(esp_matter::start(app_event_cb));
    ESP_LOGI(TAG, "Matter stack started");

    // 3. Connect to Wi-Fi
    ESP_ERROR_CHECK(wifi_connect());

    // 4. Init the Matter controller client
    using namespace esp_matter::controller;
    ESP_LOGI(TAG, "Initializing Matter controller client...");
    chip::DeviceLayer::PlatformMgr().LockChipStack();
    ESP_ERROR_CHECK(matter_controller_client::get_instance().init(
        /* node_id   */ 1,
        /* fabric_id */ 1,
        /* port      */ 5580
    ));
    ESP_LOGI(TAG, "Controller client initialized");

    // 5. Set up commissioner
    ESP_LOGI(TAG, "Setting up commissioner...");
    chip::Credentials::SetDeviceAttestationVerifier(&s_permissive_verifier);
    ESP_ERROR_CHECK(matter_controller_client::get_instance().setup_commissioner());
    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
    // 6. Run live telemetry UI loop
    telemetry_task(NULL);
}
