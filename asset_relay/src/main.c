#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/settings/settings.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/addr.h>

#include <zephyr/bluetooth/mesh.h>
#include <zephyr/bluetooth/mesh/access.h>
#include <zephyr/bluetooth/mesh/main.h>
#include <zephyr/bluetooth/mesh/msg.h>

#define BEACON_COMPANY_ID       0xFFFF
#define MESH_COMPANY_ID         0xFFFF
#define MESH_MODEL_ID           0x0001

#define OP_ASSET_REPORT         BT_MESH_MODEL_OP_3(0x01, MESH_COMPANY_ID)

#define CONTROLLER_ADDRESS      0x0001
#define APP_KEY_INDEX           0x0000
#define NET_KEY_INDEX           0x0000

static const uint8_t mesh_uuid[16] = {
        0xA1, 0x52, 0x52, 0x45,
        0x4C, 0x41, 0x59, 0x30,
        0x31, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01
};

struct asset_observation {
        uint8_t asset_id;
        uint32_t sequence;
        int8_t rssi;
};

static struct asset_observation last_asset;

/* ---------------------------------------------------------
 * Mesh Vendor Model
 * --------------------------------------------------------- */

static const struct bt_mesh_model_op vendor_ops[] = {
        BT_MESH_MODEL_OP_END,
};

static struct bt_mesh_model vendor_models[] = {
        BT_MESH_MODEL_VND(
                MESH_COMPANY_ID,
                MESH_MODEL_ID,
                vendor_ops,
                NULL,
                NULL
        ),
};

static struct bt_mesh_model sig_models[] = {
        BT_MESH_MODEL_CFG_SRV,
};

static struct bt_mesh_elem elements[] = {
        BT_MESH_ELEM(
                0,
                sig_models,
                vendor_models
        ),
};

static const struct bt_mesh_comp composition = {
        .cid = MESH_COMPANY_ID,
        .pid = 0x0001,
        .vid = 0x0001,
        .elem = elements,
        .elem_count = ARRAY_SIZE(elements),
};

/* ---------------------------------------------------------
 * Mesh Provisioning
 * --------------------------------------------------------- */

static void provisioning_complete(uint16_t net_idx, uint16_t addr)
{
        printk("\n");
        printk("========================================\n");
        printk("MESH PROVISIONING COMPLETE\n");
        printk("========================================\n");

        printk("NetKey Index : 0x%04X\n", net_idx);
        printk("Node Address : 0x%04X\n", addr);

        printk("Relay state  : %d\n",
               bt_mesh_relay_get());

        printk("BLE scanner callback is ready\n");
        printk("\n");
}

static void provisioning_reset(void)
{
        printk("\n");
        printk("Mesh reset requested\n");
        printk("Node is no longer provisioned\n");
}

static const struct bt_mesh_prov provisioning = {
        .uuid = mesh_uuid,
        .complete = provisioning_complete,
        .reset = provisioning_reset,
};

/* ---------------------------------------------------------
 * BLE Beacon Advertisement Parser
 * --------------------------------------------------------- */

static bool parse_advertisement(struct bt_data *data,
                                void *user_data)
{
        struct asset_observation *observation = user_data;

        if (data->type != BT_DATA_MANUFACTURER_DATA) {
                return true;
        }

        if (data->data_len < 7) {
                return true;
        }

        uint16_t company_id =
                data->data[0] |
                ((uint16_t)data->data[1] << 8);

        if (company_id != BEACON_COMPANY_ID) {
                return true;
        }

        observation->asset_id = data->data[2];

        observation->sequence =
                ((uint32_t)data->data[3]) |
                ((uint32_t)data->data[4] << 8) |
                ((uint32_t)data->data[5] << 16) |
                ((uint32_t)data->data[6] << 24);

        return false;
}

/* ---------------------------------------------------------
 * Send Asset Report through Bluetooth Mesh
 * --------------------------------------------------------- */

static void send_asset_report(void)
{
        int err;

        if (!bt_mesh_is_provisioned()) {
                printk("Mesh not provisioned - report not sent\n");
                return;
        }

        BT_MESH_MODEL_BUF_DEFINE(msg, OP_ASSET_REPORT, 6);

        bt_mesh_model_msg_init(&msg, OP_ASSET_REPORT);

        net_buf_simple_add_u8(&msg, last_asset.asset_id);
        net_buf_simple_add_le32(&msg, last_asset.sequence);
        net_buf_simple_add_u8(&msg, (uint8_t)last_asset.rssi);

        struct bt_mesh_msg_ctx ctx =
                BT_MESH_MSG_CTX_INIT(
                        NET_KEY_INDEX,
                        APP_KEY_INDEX,
                        CONTROLLER_ADDRESS,
                        BT_MESH_TTL_DEFAULT
                );

        err = bt_mesh_model_send(
                &vendor_models[0],
                &ctx,
                &msg,
                NULL,
                NULL
        );

        if (err) {
                printk("Mesh report send FAILED: %d\n", err);
                return;
        }

        printk("Mesh report SENT -> A\n");
}

/* ---------------------------------------------------------
 * BLE Scan Receive Callback
 * --------------------------------------------------------- */

static void scan_recv(const struct bt_le_scan_recv_info *info,
                      struct net_buf_simple *buf)
{
        struct asset_observation observation;

        observation.asset_id = 0;
        observation.sequence = 0;
        observation.rssi = info->rssi;

        bt_data_parse(buf, parse_advertisement, &observation);

        if (observation.asset_id == 0) {
                return;
        }

        last_asset = observation;

        printk("\n");
        printk("========================================\n");
        printk("ASSET BEACON DETECTED\n");
        printk("========================================\n");
        printk("Asset ID : %u\n", last_asset.asset_id);
        printk("Sequence : %u\n", last_asset.sequence);
        printk("RSSI     : %d dBm\n", last_asset.rssi);
        printk("========================================\n");

        send_asset_report();
}

/* ---------------------------------------------------------
 * Main Application
 * --------------------------------------------------------- */

int main(void)
{
        int err;

        printk("\n");
        printk("========================================\n");
        printk("       ASSET RELAY / SCANNER\n");
        printk("========================================\n");

        /* Start Bluetooth. */

        err = bt_enable(NULL);

        if (err) {
                printk("Bluetooth initialization failed: %d\n", err);
                return 0;
        }

        printk("Bluetooth initialized\n");

        /* Register scanner callback.
         * The Mesh stack may already use the scanner.
         */

        static struct bt_le_scan_cb scan_cb = {
                .recv = scan_recv,
                .timeout = NULL,
        };

        err = bt_le_scan_cb_register(&scan_cb);

        if (err) {
                printk("Scan callback registration failed: %d\n", err);
                return 0;
        }

        printk("BLE scan callback registered\n");

        /* Initialize Bluetooth Mesh. */

        err = bt_mesh_init(&provisioning, &composition);

        if (err) {
                printk("Mesh initialization failed: %d\n", err);
                return 0;
        }

        printk("Bluetooth Mesh initialized\n");

        /* Load saved Mesh settings. */

        if (IS_ENABLED(CONFIG_SETTINGS)) {
                err = settings_load();

                if (err) {
                        printk("Settings load failed: %d\n", err);
                }
        }

        printk("Provisioned : %s\n",
               bt_mesh_is_provisioned() ? "YES" : "NO");

        printk("Relay state : %d\n",
               bt_mesh_relay_get());

        /* Enable Mesh provisioning if not already provisioned. */

        if (!bt_mesh_is_provisioned()) {

                err = bt_mesh_prov_enable(
                        BT_MESH_PROV_ADV |
                        BT_MESH_PROV_GATT
                );

                if (err) {
                        printk("Provisioning enable failed: %d\n", err);
                        return 0;
                }

                printk("Waiting for Mesh provisioning...\n");
        }

        printk("BLE scanner callback ready\n");
        printk("Looking for ASSET_BEACON_01...\n");

        while (1) {

                k_sleep(K_SECONDS(10));

                printk(
                        "System alive | Mesh=%s | Relay=%d\n",
                        bt_mesh_is_provisioned()
                                ? "PROVISIONED"
                                : "NOT PROVISIONED",
                        bt_mesh_relay_get()
                );
        }

        return 0;
}
