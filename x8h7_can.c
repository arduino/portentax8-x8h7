/**
 * X8H7 CAN driver
 */

#include "x8h7_can.h"

#include <linux/can/core.h>
#include <linux/can/dev.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/freezer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/regulator/consumer.h>
#include <linux/wait.h>

#include "x8h7.h"

#define DRIVER_NAME "x8h7_can"

/* DEBUG HANDLING */
//#define DEBUG
#include "debug.h"
#ifdef DEBUG
  #define DBG_CAN_STATE(d, s) { \
    DBG_PRINT("device %s:CAN State: %d CAN controller %s\n", d, s, can_sts(s)); \
  }
#else
  #define DBG_CAN_STATE(i, s)
#endif

/**
 * Those parameters are valid for the STM32H7 CAN driver using
 * CAN classic. For CAN FD different values are needed.
 */
static const struct can_bittiming_const x8h7_can_bittiming_const = {
  .name      = DRIVER_NAME,   /* Name of the CAN controller hardware */
  .tseg1_min =   1,
  .tseg1_max = 256,
  .tseg2_min =   1,
  .tseg2_max = 128,
  .sjw_max   = 128,
  .brp_min   =   1,
  .brp_max   = 512,
  .brp_inc   =   1,
};

static void x8h7_can_tx_work_handler(struct work_struct *ws);
static void x8h7_can_error_skb(struct net_device *net, int can_id, int data1);

/**
 */
static void x8h7_can_frame_to_tx_obj(struct can_frame const *frame, union x8h7_can_frame_message *x8h7_can_msg)
{
  if (frame->can_id & CAN_EFF_FLAG)
    x8h7_can_msg->field.id  = CAN_EFF_FLAG | (frame->can_id & CAN_EFF_MASK);
  else
    x8h7_can_msg->field.id  =                (frame->can_id & CAN_SFF_MASK);

  x8h7_can_msg->field.len = (frame->can_dlc <= X8H7_CAN_FRAME_MAX_DATA_LEN) ? frame->can_dlc : X8H7_CAN_FRAME_MAX_DATA_LEN;
  memcpy(x8h7_can_msg->field.data, frame->data, x8h7_can_msg->field.len);
}

/**
 */
static char* can_sts(enum can_state sts)
{
  switch(sts){
  case CAN_STATE_ERROR_ACTIVE : return "is error active";
  case CAN_STATE_ERROR_WARNING: return "is error active, warning level is reached";
  case CAN_STATE_ERROR_PASSIVE: return "is error passive";
  case CAN_STATE_BUS_OFF      : return "went into Bus Off";
  case CAN_STATE_STOPPED      : return "is in stopped mode";
  case CAN_STATE_SLEEPING     : return "is in Sleep mode";
  default                     : return "is unknown state";
  }
}

/**
 */
static void x8h7_can_status(struct x8h7_can_priv *priv, u8 intf, u8 eflag)
{
  struct net_device  *net = priv->net;
  int                 can_id = 0;
  int                 data1 = 0;

  //DBG_PRINT("\n");

  if (intf & X8H7_CAN_STS_INT_ERR)
  {
    /* Handle overflow counters */
    if (eflag & X8H7_CAN_STS_FLG_RX_OVR)
    {
      net->stats.rx_over_errors++;
      net->stats.rx_errors++;
      can_id |= CAN_ERR_CRTL;
      data1 |= CAN_ERR_CRTL_RX_OVERFLOW;
      x8h7_can_error_skb(net, can_id, data1);
    }
    if (eflag & X8H7_CAN_STS_FLG_TX_OVR)
    {
      net->stats.tx_fifo_errors++;
      net->stats.tx_errors++;
      can_id |= CAN_ERR_CRTL;
      data1 |= CAN_ERR_CRTL_TX_OVERFLOW;
      x8h7_can_error_skb(net, can_id, data1);
    }
  }

  if (intf & X8H7_CAN_STS_INT_TX_COMPLETE) {
    DBG_PRINT("TX COMPLETE");
    net->stats.tx_packets++;
    net->stats.tx_bytes += priv->tx_len;
    priv->tx_len = 0;
    can_get_echo_skb(net, 0, NULL);
    netif_wake_queue(net);
  }

  if (intf & X8H7_CAN_STS_INT_TX_ABORT_COMPLETE)
  {
    DBG_PRINT("TX ABORT COMPLETE");
  }

  if (intf & X8H7_CAN_STS_INT_TX_FIFO_EMPTY)
  {
    DBG_PRINT("TX FIFO EMPTY");
  }
}

/**
 */
static void x8h7_can_hook(void *arg, x8h7_pkt_t *pkt)
{
  struct x8h7_can_priv  *priv = (struct x8h7_can_priv*)arg;

  switch(pkt->opcode) {
  case X8H7_CAN_OC_RECV:
    if (pkt->size < X8H7_CAN_HEADER_SIZE) {
      DBG_ERROR("received packed is too short (%d)\n", pkt->size);
      return;
    } else {
      struct sk_buff   *skb;
      struct can_frame *frame;
      union x8h7_can_frame_message x8h7_can_msg;

      skb = alloc_can_skb(priv->net, &frame);
      if (!skb) {
        dev_err(priv->dev, "cannot allocate RX skb\n");
        priv->net->stats.rx_dropped++;
        return;
      }

      /* Copy header from raw byte-stream onto union. */
      memcpy(x8h7_can_msg.buf, pkt->data, X8H7_CAN_HEADER_SIZE);

      /* Extract can_id and can_dlc. Note: x8h7_can_frame_message uses the exact
       * same flags for signaling extended/standard id mode or remote
       * retransmit request as struct can_frame.
       */

      frame->can_id  = x8h7_can_msg.field.id;
      frame->can_dlc = x8h7_can_msg.field.len;

      DBG_PRINT("received data %X %X, copying to %X\n", frame->can_id, frame->can_dlc, frame->data);

      memcpy(frame->data, pkt->data + X8H7_CAN_HEADER_SIZE, frame->can_dlc);

      priv->net->stats.rx_packets++;
      priv->net->stats.rx_bytes += frame->can_dlc;
      netif_rx(skb);
    }
    break;
  case X8H7_CAN_OC_STS:
    DBG_PRINT("received status %02X %02X\n", pkt->data[0], pkt->data[1]);
    x8h7_can_status(priv, pkt->data[0], pkt->data[1]);
    break;
  }
}

/*
 * device (auto-)restart mechanism runs in a timer context =>
 * MUST handle restart with asynchronous spi transfers (if any)
 */
static int x8h7_can_restart(struct net_device *net)
{
  struct x8h7_can_priv *priv = netdev_priv(net);
  int err = 0;

  DBG_PRINT("\n");

  /* @TODO: notify external hw? */
  DBG_PRINT("@TODO: notify external hw?\n");

  /* finally MUST update can state */
	priv->can.state = CAN_STATE_ERROR_ACTIVE;

  /* netdev queue can be awaken now */
	netif_wake_queue(net);

  return err;
}

/**
 */
static int x8h7_can_hw_setup(struct x8h7_can_priv *priv)
{
  struct can_bittiming *bt = &priv->can.bittiming;
  union x8h7_can_init_message x8h7_msg;

  DBG_PRINT("bitrate: %d, sample_point: %d, tq: %d, prop_seg: %d, phase_seg1: %d, phase_seg2: %d, sjw: %d, brp: %d, freq: %d ctrlmode: %08X\n",
            bt->bitrate,
            bt->sample_point,
            bt->tq,
            bt->prop_seg,
            bt->phase_seg1,
            bt->phase_seg2,
            bt->sjw,
            bt->brp,
            priv->can.clock.freq,
            priv->can.ctrlmode);

  x8h7_msg.field.baud_rate_prescaler = bt->brp;
  x8h7_msg.field.time_segment_1      = bt->prop_seg + bt->phase_seg1 - bt->phase_seg2;
  x8h7_msg.field.time_segment_2      = can_bit_time(bt) - x8h7_msg.field.time_segment_1 - CAN_SYNC_SEG;
  x8h7_msg.field.sync_jump_width     = bt->sjw;

  DBG_PRINT("baud_rate_prescaler: %d, time_segment_1: %d, time_segment_2: %d, sync_jump_width: %d\n",
            x8h7_msg.field.baud_rate_prescaler,
            x8h7_msg.field.time_segment_1,
            x8h7_msg.field.time_segment_2,
            x8h7_msg.field.sync_jump_width);

  x8h7_pkt_send_sync(priv->periph, X8H7_CAN_OC_INIT, sizeof(x8h7_msg.buf), x8h7_msg.buf);

  return 0;
}

/**
 */
static int x8h7_can_hw_stop(struct x8h7_can_priv *priv)
{
  x8h7_pkt_send_sync(priv->periph, X8H7_CAN_OC_DEINIT, 0, NULL);

  return 0;
}

/**
 */
static int x8h7_can_set_normal_mode(struct x8h7_can_priv *priv)
{
  DBG_PRINT("\n");

  /* Enable interrupts */
  x8h7_hook_set(priv->periph, x8h7_can_hook, priv);

  return 0;
}

/**
 */
static void x8h7_can_error_skb(struct net_device *net, int can_id, int data1)
{
  struct sk_buff *skb;
  struct can_frame *frame;

  DBG_PRINT("\n");
  skb = alloc_can_err_skb(net, &frame);
  if (skb) {
    frame->can_id |= can_id;
    frame->data[1] = data1;
    netif_rx(skb);
  } else {
    netdev_err(net, "cannot allocate error skb\n");
  }
}

/**
 */
static int x8h7_can_open(struct net_device *net)
{
  struct x8h7_can_priv *priv = netdev_priv(net);
  int                   ret;

  DBG_PRINT("\n");

  ret = open_candev(net);
  if (ret) {
    DBG_ERROR("unable to set initial baudrate!\n");
    return ret;
  }

  priv->tx_len  = 0;

  priv->wq = alloc_workqueue("x8h7_can_wq", WQ_FREEZABLE | WQ_MEM_RECLAIM, 0);
  if (!priv->wq) {
    ret = -ENOMEM;
    goto out_clean;
  }
  INIT_WORK(&priv->work, x8h7_can_tx_work_handler);

  mutex_init(&priv->lock);

  ret = x8h7_can_hw_stop(priv);
  if (ret) {
    goto out_free_wq;
  }
  ret = x8h7_can_hw_setup(priv);
  if (ret) {
    goto out_free_wq;
  }
  ret = x8h7_can_set_normal_mode(priv);
  if (ret) {
    goto out_free_wq;
  }

  netif_start_queue(net);

  return 0;

out_free_wq:
  destroy_workqueue(priv->wq);
out_clean:
  x8h7_hook_set(priv->periph, NULL, NULL);
  close_candev(net);
  return ret;
}

/*
 * Called by netdev to close the corresponding CAN interface.
 */
static int x8h7_can_stop(struct net_device *net)
{
  struct x8h7_can_priv *priv = netdev_priv(net);

  DBG_PRINT("\n");

  /* Notify upper level */
  netif_stop_queue(net);
  close_candev(net);

  /* Notify ext. hw to stop can peripheral */
  x8h7_can_hw_stop(priv);

  /* Free priv. resources */
  mutex_lock(&priv->lock);
  x8h7_hook_set(priv->periph, NULL, NULL);
  destroy_workqueue(priv->wq);
  priv->wq = NULL;

  priv->can.state = CAN_STATE_STOPPED;
  mutex_unlock(&priv->lock);

  return 0;
}

/**
 */
static netdev_tx_t x8h7_can_start_xmit(struct sk_buff *skb,
                                       struct net_device *net)
{
  struct x8h7_can_priv        *priv = netdev_priv(net);
  const struct device         *dev = priv->dev;
  struct can_frame            *frame;

  DBG_PRINT("\n");

  if (can_dropped_invalid_skb(net, skb))
    return NETDEV_TX_OK;

  netif_stop_queue(net);

  frame = (struct can_frame *)skb->data;
  x8h7_can_frame_to_tx_obj(frame, &priv->tx_frame);
  can_put_echo_skb(skb, net, 0, 0);
  queue_work(priv->wq, &priv->work);

  return NETDEV_TX_OK;
}

/**
 */
static void x8h7_can_tx_work_handler(struct work_struct *ws)
{
  struct x8h7_can_priv *priv = container_of(ws, struct x8h7_can_priv, work);

#ifdef DEBUG
  char  data_str[X8H7_CAN_FRAME_MAX_DATA_LEN * 4];
  int   i;
  int   len;

  i = 0; len = 0;
  for (i = 0; (i < priv->tx_frame.field.len) && (len < sizeof(data_str)); i++)
    len += snprintf(data_str + len, sizeof(data_str) - len, " %02X", priv->tx_frame.field.data[i]);
  DBG_PRINT("Send CAN frame to H7: id = %08X, len = %d, data = [%s ]\n", priv->tx_frame.field.id, priv->tx_frame.field.len, data_str);
#endif

  priv->tx_len = priv->tx_frame.field.len;

  x8h7_pkt_send_sync(priv->periph,
                     X8H7_CAN_OC_SEND,
                     X8H7_CAN_HEADER_SIZE + priv->tx_frame.field.len, /* Send 4-Byte ID, 1-Byte Length and the required number of data bytes. */
                     priv->tx_frame.buf);
}

/**
 */
static int x8h7_can_hw_do_set_bittiming(struct net_device *net)
{
  struct x8h7_can_priv *priv = netdev_priv(net);
  struct can_bittiming *bt = &priv->can.bittiming;
  union x8h7_can_bittiming_message x8h7_msg;

  DBG_PRINT("\n");

  x8h7_msg.field.baud_rate_prescaler = bt->brp;
  x8h7_msg.field.time_segment_1      = bt->prop_seg + bt->phase_seg1 - bt->phase_seg2;
  x8h7_msg.field.time_segment_2      = can_bit_time(bt) - x8h7_msg.field.time_segment_1 - CAN_SYNC_SEG;
  x8h7_msg.field.sync_jump_width     = bt->sjw;

  DBG_PRINT("baud_rate_prescaler: %d, time_segment_1: %d, time_segment_2: %d, sync_jump_width: %d\n",
            x8h7_msg.field.baud_rate_prescaler,
            x8h7_msg.field.time_segment_1,
            x8h7_msg.field.time_segment_2,
            x8h7_msg.field.sync_jump_width);

  x8h7_pkt_send_sync(priv->periph, X8H7_CAN_OC_BITTIM, sizeof(x8h7_msg.buf), x8h7_msg.buf);

  return 0;
}

/*
 * candev callback used to change CAN mode.
 * Warning: this is called from a timer context!
 */
static int x8h7_can_do_set_mode(struct net_device *net, enum can_mode mode)
{
  int err = 0;

  DBG_PRINT("\n");

  switch (mode) {
  case CAN_MODE_START:
    err = x8h7_can_restart(net);
    if (err)
      netdev_err(net, "couldn't start device (err %d)\n",
				   err);
    break;
  default:
    return -EOPNOTSUPP;
  }

  return err;
}

/**
 */
static int x8h7_can_do_get_berr_counter(const struct net_device *net,
                                        struct can_berr_counter *bec)
{
  //@TODO: to be read from device
  bec->txerr = 0;
  bec->rxerr = 0;

  return 0;
}

/**
 */
static const struct net_device_ops x8h7_can_netdev_ops = {
  .ndo_open       = x8h7_can_open,
  .ndo_stop       = x8h7_can_stop,
  .ndo_start_xmit = x8h7_can_start_xmit,
  //.ndo_change_mtu = can_change_mtu,
};

/**
 */
static int x8h7_can_hw_config_filter(struct x8h7_can_priv *priv,
                                     uint32_t const idx,
                                     uint32_t const id,
                                     uint32_t const mask)
{
  union x8h7_can_filter_message x8h7_msg;

  x8h7_msg.field.idx  = idx;
  x8h7_msg.field.id   = id;
  x8h7_msg.field.mask = mask;

  DBG_PRINT("SEND idx %X, id %X, mask %X\n", idx, id, mask);

  x8h7_pkt_send_sync(priv->periph, X8H7_CAN_OC_FLT, sizeof(x8h7_msg.buf), x8h7_msg.buf);

  return 0;
}

/**
 * Standard id filter show
 */
static ssize_t x8h7_can_sf_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
  struct x8h7_can_priv *priv = netdev_priv(to_net_dev(dev));
  int                   len;
  int                   i;

  len = 0;
  for (i = 0; i < X8H7_STD_FLT_MAX; i++)
  {
    if (priv->std_flt[i].can_mask) {
      len += snprintf(buf + len, PAGE_SIZE - len,
                      "%02X %08X %08X\n",
                      i, priv->std_flt[i].can_id, priv->std_flt[i].can_mask);
    }
  }
  return len;
}

/**
 * Standard id filter set
 */
static ssize_t x8h7_can_sf_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf, size_t count)
{
  struct x8h7_can_priv *priv = netdev_priv(to_net_dev(dev));
  uint32_t              idx;
  uint32_t              id;
  uint32_t              mask;
  int                   ret;

  ret = sscanf(buf, "%x %x %x", &idx, &id, &mask);

  if (ret != 3) {
    DBG_ERROR("invalid num of params\n");
    return -EINVAL;
  }

  if ((idx >= X8H7_STD_FLT_MAX) ||
      (id & ~0x7FF) || (mask & ~0x7FF)) {
    DBG_ERROR("invalid params\n");
    return -EINVAL;
  }

  ret = x8h7_can_hw_config_filter(priv, idx, id, mask);
  if (ret) {
    DBG_ERROR("set filter\n");
    return -EIO;
  }

  priv->std_flt[idx].can_id   = id;
  priv->std_flt[idx].can_mask = mask;

  return count;
}

/**
 * Extended id filter show
 */
static ssize_t x8h7_can_ef_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
  struct x8h7_can_priv *priv = netdev_priv(to_net_dev(dev));
  int                   len;
  int                   i;

  len = 0;
  for (i = 0; i < X8H7_EXT_FLT_MAX; i++)
  {
    if (priv->ext_flt[i].can_mask) {
      len += snprintf(buf + len, PAGE_SIZE - len,
                      "%02X %08X %08X\n",
                      i, priv->ext_flt[i].can_id, priv->ext_flt[i].can_mask);
    }
  }
  return len;
}

/**
 * Extended id filter set
 */
static ssize_t x8h7_can_ef_store(struct device *dev,
                               struct device_attribute *attr,
                               const char *buf, size_t count)
{
  struct x8h7_can_priv *priv = netdev_priv(to_net_dev(dev));
  int                   ret;
  uint32_t              idx;
  uint32_t              id;
  uint32_t              mask;

  ret = sscanf(buf, "%x %x %x", &idx, &id, &mask);

  if (ret != 3) {
    DBG_ERROR("invalid num of params\n");
    return -EINVAL;
  }

  if ((idx >= X8H7_EXT_FLT_MAX) ||
      (id & ~0x1FFFFFFF) || (mask & ~0x1FFFFFFF)) {
    DBG_ERROR("invalid params\n");
    return -EINVAL;
  }

  ret = x8h7_can_hw_config_filter(priv, idx, (CAN_EFF_FLAG | id), mask);
  if (ret) {
    DBG_ERROR("set filter\n");
    return -EIO;
  }

  priv->ext_flt[idx].can_id   = id;
  priv->ext_flt[idx].can_mask = mask;

  return count;
}

/**
 * Status show
 */
static ssize_t x8h7_can_sts_show(struct device *dev,
                                struct device_attribute *attr, char *buf)
{
  struct x8h7_can_priv *priv = netdev_priv(to_net_dev(dev));
  int                   len;

  len = snprintf(buf, PAGE_SIZE,
                  "status         %d %s\n"
                  "error warning  %d\n"
                  "error passive  %d\n"
                  "bus off        %d\n"
                  "tx packets     %ld\n"
                  "tx bytes       %ld\n"
                  "rx packets     %ld\n"
                  "rx bytes       %ld\n"
                  "rx over_errors %ld\n"
                  "rx errors      %ld\n"
                  "rx dropped     %ld\n",
                  priv->can.state, can_sts(priv->can.state),
                  priv->can.can_stats.error_warning,
                  priv->can.can_stats.error_passive,
                  priv->can.can_stats.bus_off,
                  priv->net->stats.tx_packets,
                  priv->net->stats.tx_bytes,
                  priv->net->stats.rx_packets,
                  priv->net->stats.rx_bytes,
                  priv->net->stats.rx_over_errors,
                  priv->net->stats.rx_errors,
                  priv->net->stats.rx_dropped);
  return len;
}

static DEVICE_ATTR(std_flt, 0644, x8h7_can_sf_show, x8h7_can_sf_store);
static DEVICE_ATTR(ext_flt, 0644, x8h7_can_ef_show, x8h7_can_ef_store);
static DEVICE_ATTR(status , 0644, x8h7_can_sts_show, NULL);

static struct attribute *x8h7_can_sysfs_attrs[] = {
  &dev_attr_std_flt.attr,
  &dev_attr_ext_flt.attr,
  &dev_attr_status.attr,
  //....
  NULL,
};

static const struct attribute_group x8h7_can_sysfs_attr_group = {
  .name = "x8h7can",
  .attrs = (struct attribute **)x8h7_can_sysfs_attrs,
};

/**
 */
static int x8h7_can_probe(struct platform_device *pdev)
{
  struct net_device    *net;
  struct x8h7_can_priv *priv;
  int                   err;

  u32 clock_freq = 100000000;

  DBG_PRINT("\n");

  if (pdev->dev.of_node) {
    of_property_read_u32(pdev->dev.of_node, "clock-frequency", &clock_freq);
    DBG_PRINT("fdcan_clk = %d", clock_freq);
  }

  net = alloc_candev(sizeof(struct x8h7_can_priv), 1);
  if (!net) {
    return -ENOMEM;
  }

  net->netdev_ops = &x8h7_can_netdev_ops;
  net->flags |= IFF_ECHO;
  net->sysfs_groups[0] = &x8h7_can_sysfs_attr_group;

  priv = netdev_priv(net);

  priv->can.clock.freq          = clock_freq;
  priv->can.bittiming_const     = &x8h7_can_bittiming_const;
  priv->can.do_set_bittiming    = x8h7_can_hw_do_set_bittiming;
  priv->can.do_set_mode         = x8h7_can_do_set_mode;
  priv->can.do_get_berr_counter = x8h7_can_do_get_berr_counter;
  priv->can.ctrlmode_supported  = CAN_CTRLMODE_LOOPBACK      |
                                  CAN_CTRLMODE_LISTENONLY    |
                                  CAN_CTRLMODE_3_SAMPLES     ;
  priv->net = net;

  platform_set_drvdata(pdev, priv);

  SET_NETDEV_DEV(net, &pdev->dev);

  err = register_candev(net);
  if (err) {
    dev_err(&pdev->dev, "registering netdev failed\n");
    goto failed_register;
  }
  DBG_PRINT("net device registered %s, "
            "ifindex: %d, if_port %d, dev_id: %d, dev_port %d\n",
            net->name, net->ifindex, net->if_port, net->dev_id, net ->dev_port);

  if (net->name[3] == '0') {
    priv->periph = X8H7_CAN1_PERIPH;
  } else {
    priv->periph = X8H7_CAN2_PERIPH;
  }
  priv->dev = &pdev->dev;
  DBG_PRINT("periph: %d DONE\n", priv->periph);

  netdev_info(net, "X8H7 CAN successfully initialized.\n");

  return 0;

failed_register:
  DBG_ERROR("\n");
  free_candev(net);
  return err;
}

/**
 */
static int x8h7_can_remove(struct platform_device *pdev)
{
  struct x8h7_can_priv *priv = platform_get_drvdata(pdev);
  struct net_device    *net = priv->net;

  unregister_candev(net);
  free_candev(net);

  return 0;
}

/**
 */
static const struct of_device_id x8h7_can_of_match[] = {
  { .compatible = "portenta,x8h7_can", },
  { /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, x8h7_can_of_match);

/**
 */
static const struct platform_device_id x8h7_can_id_table[] = {
  { .name = "x8h7_can", },
  { /* sentinel */ },
};
MODULE_DEVICE_TABLE(platform, x8h7_can_id_table);

/**
 */
static struct platform_driver x8h7_can_driver = {
  .driver = {
    .name           = DRIVER_NAME,
    .of_match_table = x8h7_can_of_match,
  },
  .probe    = x8h7_can_probe,
  .remove   = x8h7_can_remove,
  .id_table = x8h7_can_id_table,
};

module_platform_driver(x8h7_can_driver);

MODULE_AUTHOR("Massimiliano Agneni <massimiliano@iptronix.com");
MODULE_DESCRIPTION("Arduino Portenta X8 CAN driver");
MODULE_LICENSE("GPL v2");
