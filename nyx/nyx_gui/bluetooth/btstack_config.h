#ifndef NOVA_BTSTACK_CONFIG_H
#define NOVA_BTSTACK_CONFIG_H
/* Classic HID only. All stack pools are fixed: no BLE, audio or malloc. */
#define ENABLE_CLASSIC
/* Nova knows the Joy-Con HID channels and parses reports itself. Do not
 * require an SDP descriptor query before opening the standard HID channels. */
#define NOVA_HID_KNOWN_PSMS
#define HAVE_EMBEDDED_TIME_MS
#define HCI_ACL_PAYLOAD_SIZE 1024
#define MAX_NR_HCI_CONNECTIONS 2
#define MAX_NR_L2CAP_CHANNELS 8
#define MAX_NR_L2CAP_SERVICES 2
#define MAX_NR_HID_HOST_CONNECTIONS 2
#define MAX_NR_SDP_CLIENT_QUERY_REQUESTS 2
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#endif
