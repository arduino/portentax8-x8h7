/**
 * Portenta X8H7
 */
#ifndef __X8H7_H
#define __X8H7_H

#define X8H7_RX_TIMEOUT (HZ/10)

#define X8H7_BUF_SIZE   (256)
#define FIXED_PACKET_LEN  X8H7_BUF_SIZE
#define X8H7_PKT_SIZE   (X8H7_BUF_SIZE - 8)

typedef struct {
  uint8_t   peripheral;
  uint8_t   opcode;
  uint16_t  size;
  uint8_t   data[X8H7_PKT_SIZE];
} x8h7_pkt_t;

typedef void (*x8h7_hook_t)(void *priv, x8h7_pkt_t *pkt);

int x8h7_pkt_send_sync(uint8_t peripheral, uint8_t opcode, uint16_t size, void *data);
int x8h7_pkt_send_defer(uint8_t peripheral, uint8_t opcode, uint16_t size, void *data);
int x8h7_pkt_send_now(void);
int x8h7_hook_set(uint8_t idx, x8h7_hook_t hook, void *priv);
int x8h7_dbg_set(void (*hook)(void*, uint8_t*, uint16_t), void *priv);
#endif  /* __X8H7_H */
