#include "contiki.h"
#include "net/routing/routing.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/sixtop/sixtop.h"
#include "sf-simple.h"
#include "sys/log.h"
#include "net/ipv6/simple-udp.h"
#include "sys/rtimer.h"
#include "sys/node-id.h"
#include "net/routing/rpl-lite/rpl.h"
#include "net/ipv6/uip-debug.h"
#include "net/nbr-table.h"
#include <stdio.h>
#include "project-conf.h"

/************************************************
 *                  Constants                   *
 ************************************************/
#define LOG_MODULE "Coordinator"
#define LOG_LEVEL LOG_LEVEL_DBG
#define UDP_PORT 1234
#define MAX_NODES 16
#define CHECK_INTERVAL (CLOCK_SECOND * 5)

/************************************************
 *                  Structs                     *
 ************************************************/
/* Structure to hold statistics for each node */
typedef struct {
  uint16_t tx_count;        // Transmission count
  uint16_t rx_count;        // Reception count
  uip_ipaddr_t node_addr;   // Node IP address
  uip_ipaddr_t parent_addr; // Parent IP address
  uint16_t ping_sent;       // Total PINGs sent
  uint16_t pong_received;   // Total PONGs received
  uint16_t rtt;             // Round-Trip Time in ms
  int16_t temperature;      // Temperature in Celsius
  int16_t rssi;             // RSSI for received packet
} node_stats_t;

/* Structure for sensor payload including PING data */
typedef struct {
  uint8_t node_id;          // Node ID
  uint16_t tx_count;        // Transmission count
  uint16_t rx_count;        // Reception count (PONG received)
  uint64_t send_time;       // Send time (network uptime ticks)
  int16_t temperature;      // Temperature in Celsius
  uip_ipaddr_t parent_addr; // Parent IP address
  uint16_t ping_sent;       // Total PINGs sent
  uint16_t pong_received;   // Total PONGs received
  uint16_t rtt;             // Round-Trip Time in ms
} sensor_payload_t;

/* Structure for PING message */
typedef struct {
  uint8_t msg_type; // 'P' for PING
  uint8_t node_id;
  uint16_t seq_num;
} ping_message_t;

/* Structure for PONG message */
typedef struct {
  uint8_t msg_type; // 'O' for PONG
  uint8_t node_id;
  uint16_t seq_num;
} pong_message_t;

/************************************************
 *              Global variables                *
 ************************************************/
static node_stats_t node_stats[MAX_NODES];
static struct simple_udp_connection udp_conn;
static int16_t nodes_to_check[] = {8, 15, 145}; // Valid node IDs

/************************************************
 *                  Functions                   *
 ************************************************/
/* Map a node ID to a valid index in node_stats */
static int get_node_index(uint16_t node_id) {
  for(int i = 0; i < sizeof(nodes_to_check) / sizeof(nodes_to_check[0]); i++) {
    if(nodes_to_check[i] == node_id) {
      return i; // Return the index if node_id matches
    }
  }
  return -1; // Return -1 if node_id is not valid
}

/* Function to print routing table and node statistics */
static void print_routing_table() {
  uip_ipaddr_t coordinator_addr;
  if(NETSTACK_ROUTING.get_root_ipaddr(&coordinator_addr)) {
    printf("Coordinator: ");
    uip_debug_ipaddr_print(&coordinator_addr);
    printf("\r\nRouting and Node Statistics:\r\n");
  } else {
    printf("Coordinator address unavailable.\r\n");
  }

  for(int j = 0; j < sizeof(nodes_to_check) / sizeof(nodes_to_check[0]); j++) {
    int node_id = nodes_to_check[j];
    int index = get_node_index(node_id);
    if(index >= 0 && node_stats[index].rx_count > 0) {
      printf("Node ID %d [", node_id);
      uip_debug_ipaddr_print(&node_stats[index].node_addr);
      printf("] via ");
      if(uip_is_addr_unspecified(&node_stats[index].parent_addr)) {
        printf("root");
      } else {
        uip_debug_ipaddr_print(&node_stats[index].parent_addr);
      }

      int packet_loss = (node_stats[index].ping_sent > 0) ? 
                  (int)((1.0 - ((float)node_stats[index].pong_received / node_stats[index].ping_sent)) * 100) : 100;


      printf(" | TX: %u | RX: %u | PRR: %d%% | Temp: %dC | RSSI: %d | PING Sent: %u | PONG Received: %u | Packet Loss: %d%% | RTT: %u ms\r\n",
             node_stats[index].tx_count,
             node_stats[index].rx_count,
             (node_stats[index].tx_count > 0) ? 
               (int)((float)node_stats[index].rx_count / node_stats[index].tx_count * 100) : 0,
             node_stats[index].temperature,
             node_stats[index].rssi,
             node_stats[index].ping_sent,
             node_stats[index].pong_received,
             packet_loss,
             node_stats[index].rtt);
    }
  }
}

/* UDP receive callback function */
static void udp_rx_callback(struct simple_udp_connection *c,
                            const uip_ipaddr_t *sender_addr,
                            uint16_t sender_port,
                            const uip_ipaddr_t *receiver_addr,
                            uint16_t receiver_port,
                            const uint8_t *data,
                            uint16_t datalen) {
  /* Check if the message is a PING request */
  if(datalen == sizeof(ping_message_t)) {
    ping_message_t ping_msg;
    memcpy(&ping_msg, data, sizeof(ping_message_t));

    if (ping_msg.msg_type == 'P') {
      pong_message_t pong_msg;
      pong_msg.msg_type = 'O';
      pong_msg.node_id = ping_msg.node_id;
      pong_msg.seq_num = ping_msg.seq_num;

      simple_udp_sendto(&udp_conn, &pong_msg, sizeof(pong_msg), sender_addr);

      LOG_INFO("PONG sent to Node %u [", ping_msg.node_id);
      uip_debug_ipaddr_print(sender_addr);
      LOG_INFO_("] seq_num: %u\r\n", ping_msg.seq_num);
      return;
    }
  }

  /* Check if the message is a sensor_payload_t */
  if(datalen == sizeof(sensor_payload_t)) {
    sensor_payload_t received_data;
    memcpy(&received_data, data, sizeof(sensor_payload_t));

    int index = get_node_index(received_data.node_id);
    if(index < 0) {
      LOG_ERR("Invalid node ID %u\r\n", received_data.node_id);
      return;
    }

    /* Update node statistics */
    node_stats[index].tx_count = received_data.tx_count;
    node_stats[index].rx_count++;
    node_stats[index].temperature = received_data.temperature;
    node_stats[index].ping_sent = received_data.ping_sent;
    node_stats[index].pong_received = received_data.pong_received;
    node_stats[index].rtt = received_data.rtt;
    uip_ipaddr_copy(&node_stats[index].node_addr, sender_addr);
    uip_ipaddr_copy(&node_stats[index].parent_addr, &received_data.parent_addr);

    /* Get RSSI of received packet */
    node_stats[index].rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);

    /* Calculate Packet Reception Ratio (PRR) */
    int prr = (node_stats[index].tx_count > 0) ? 
              (int)((float)node_stats[index].rx_count / node_stats[index].tx_count * 100) : 0;

    /* Log sensor data and PING data */
    LOG_INFO("Node %u | TX: %u | RX: %u | PRR: %d%% | Temp: %dC | RSSI: %d | PING Sent: %u | PONG Received: %u | RTT: %u ms\r\n",
             received_data.node_id,
             received_data.tx_count,
             node_stats[index].rx_count,
             prr,
             received_data.temperature,
             node_stats[index].rssi,
             received_data.ping_sent,
             received_data.pong_received,
             received_data.rtt);
  } else {
    LOG_ERR("Received packet with unexpected size: %u bytes\r\n", datalen);
  }
}

/************************************************
 *                  Processes                   *
 ************************************************/
PROCESS(coordinator_process, "Coordinator Process");
AUTOSTART_PROCESSES(&coordinator_process);

PROCESS_THREAD(coordinator_process, ev, data) {
  static struct etimer timer;
  PROCESS_BEGIN();

  LOG_INFO("Starting coordinator node...\r\n");
  NETSTACK_ROUTING.root_start();
  sixtop_add_sf(&sf_simple_driver);
  NETSTACK_MAC.on();

  /* Register UDP connection */
  simple_udp_register(&udp_conn, UDP_PORT, NULL, UDP_PORT, udp_rx_callback);

  /* Set timer for periodic checks */
  etimer_set(&timer, CHECK_INTERVAL);
  while(1) {
    PROCESS_YIELD();

    if(etimer_expired(&timer)) {
      print_routing_table();
      etimer_reset(&timer);
    }
  }

  PROCESS_END();
}
