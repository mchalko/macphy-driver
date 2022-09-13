#include "open_alliance.h"

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/interrupt.h>

static const u8 oa_parity_lut[256] = {
        1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,1,
        0,1,0,0,1,1,0,0,1,0,1,1,0,0,1,1,0,1,0,
        0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,0,
        1,1,0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,
        0,1,1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,
        1,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,1,
        1,0,1,0,0,1,1,0,0,1,0,1,1,0,0,1,1,0,1,
        0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,
        0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,0,1,1,
        0,1,0,0,1,0,1,1,0,1,0,0,1,1,0,0,1,0,1,
        1,0,1,0,0,1,0,1,1,0,0,1,1,0,1,0,0,1,0,
        1,1,0,1,0,0,1,1,0,0,1,0,1,1,0,0,1,1,0,
        1,0,0,1,1,0,0,1,0,1,1,0,1,0,0,1,0,1,1,
        0,0,1,1,0,1,0,0,1
};

static u32 oa_convert_control_header(struct oa_control_header_t header){
    u32 result = 0;

    result |= FIELD_PREP(OA_FIELD_WNR, header.write_not_read);
    result |= FIELD_PREP(OA_FIELD_AID, header.address_increment_disable);
    result |= FIELD_PREP(OA_FIELD_MMS, header.memory_map_selector);
    result |= FIELD_PREP(OA_FIELD_ADDR, header.address);
    result |= FIELD_PREP(OA_FIELD_LEN, header.length);
    result |= FIELD_PREP(OA_FIELD_PARITY, header.parity);

    return cpu_to_be32(result);
}

static u32 oa_convert_tx_chunk_header(struct oa_tx_chunk_header_t header){
    u32 result = 0;

    result |= FIELD_PREP(OA_FIELD_DNC,  header.data_not_control);
    result |= FIELD_PREP(OA_FIELD_NORX, header.no_receive);
    result |= FIELD_PREP(OA_FIELD_DV,   header.data_valid);
    result |= FIELD_PREP(OA_FIELD_SV,   header.start_valid);
    result |= FIELD_PREP(OA_FIELD_SWO,  header.start_word_offset);
    result |= FIELD_PREP(OA_FIELD_EV,   header.end_valid);
    result |= FIELD_PREP(OA_FIELD_EBO,  header.end_byte_offset);
    result |= FIELD_PREP(OA_FIELD_PARITY, header.parity);

    return cpu_to_be32(result);
}

static struct oa_rx_chunk_footer_t oa_convert_rx_chunk_footer(u32 value){
    struct oa_rx_chunk_footer_t footer = {0};
    value = be32_to_cpu(value);

    footer.data_valid = FIELD_GET(OA_FIELD_DV, value);
    footer.start_valid = FIELD_GET(OA_FIELD_SV, value);
    footer.start_word_offset = FIELD_GET(OA_FIELD_SWO, value);
    footer.end_valid = FIELD_GET(OA_FIELD_EV, value);
    footer.end_byte_offset = FIELD_GET(OA_FIELD_EBO, value);
    footer.parity = FIELD_GET(OA_FIELD_PARITY, value);
    footer.frame_drop = FIELD_GET(OA_FIELD_FD, value);
    footer.receive_chunks_available = FIELD_GET(OA_FIELD_RCA, value);
    footer.transmit_credits = FIELD_GET(OA_FIELD_TXC, value);
    footer.configuration_synchronized = FIELD_GET(OA_FIELD_SYNC, value);
    footer.extended_status = FIELD_GET(OA_FIELD_EXST, value);

    return footer;
}

static u8 oa_calc_parity(u32 input){
    input ^= (input >> 16);
    input ^= (input >>  8);
    return oa_parity_lut[input & 0xFF];
}

static int oa_process_received_chunk(struct open_alliance* oa, struct oa_rx_chunk_t* chunk){
    struct sk_buff* skb;
    u32 offset;
    u32 data_size = OA_CHUNK_DATA_SIZE;
    u32 frame_size;

    if(chunk->footer.bits.start_valid) {
        oa->frame_assembly_idx = 0;
        oa->frame_assembly_err = false;
    }

    if(oa->frame_assembly_idx >= OA_FRAME_ASSEMBLY_CHUNKS)
        oa->frame_assembly_err = true;
    if(oa->frame_assembly_err)
        return 0;

    if (chunk->footer.bits.end_valid) {
        if(chunk->footer.bits.frame_drop)
            return 0;
        data_size = chunk->footer.bits.end_byte_offset + 1;
    }

    offset = oa->frame_assembly_idx * OA_CHUNK_DATA_SIZE;
    memcpy (&oa->frame_assembly[offset], chunk->data, data_size);
    oa->frame_assembly_idx++;

    if(chunk->footer.bits.end_valid){
        frame_size = offset + data_size;
        skb = netdev_alloc_skb (oa->netdev, frame_size);
        if (!skb)
            return -ENOMEM;
        memcpy (skb_put (skb, frame_size), oa->frame_assembly, frame_size);
        skb->protocol = eth_type_trans (skb, oa->netdev);

        return netif_receive_skb (skb);
    }

    return 0;
}

static int oa_transfer_chunks(struct open_alliance* oa, bool force){
    struct oa_tx_chunk_t* chunk_tx;
    struct oa_rx_chunk_t* chunk_rx;
    struct spi_transfer t = { 0 };
    u32 chunks_to_send = 0;
    u32 offset;
    bool repeat = false;
    int i;
    int ret;

    if (kfifo_len(&oa->tx_chunks_fifo)){
        if(oa->tx_credits) {
            chunks_to_send = min(kfifo_len(&oa->tx_chunks_fifo), oa->tx_credits);
        }else {
            chunks_to_send = 1;
            repeat = true;
        }
    }
    if(oa->rx_available > chunks_to_send)
        chunks_to_send = oa->rx_available;
    if(!chunks_to_send && force) // one chunk forced by int
        chunks_to_send = 1;
    if(chunks_to_send > OA_TRX_BUFFER_CHUNKS)
        chunks_to_send = OA_TRX_BUFFER_CHUNKS;
    if(!chunks_to_send)
        return 0;


    for(i = 0; i < chunks_to_send; i++){
        offset = i * OA_CHUNK_SIZE;
        chunk_tx = (struct oa_tx_chunk_t*) &oa->tx_data[offset];

        if(!kfifo_is_empty(&oa->tx_chunks_fifo) && oa->tx_credits){
            ret = kfifo_out_spinlocked(&oa->tx_chunks_fifo, chunk_tx, 1,
                                       &oa->tx_lock);
            if (ret < 0)
                return ret;
            oa->tx_credits--;
        }else{
            chunk_tx->header.value = 0;
            chunk_tx->header.bits.data_not_control = 1;
        }

        chunk_tx->header.bits.parity = 0;
        chunk_tx->header.bits.parity = oa_calc_parity (chunk_tx->header.value);
        chunk_tx->header.value = oa_convert_tx_chunk_header(chunk_tx->header.bits);
    }


    t.rx_buf = oa->rx_data;
    t.tx_buf = oa->tx_data;
    t.len = chunks_to_send * OA_CHUNK_SIZE;

    ret = spi_sync_transfer(oa->spidev, &t, 1);
    if (ret < 0)
        return ret;

    for (i = 0; i < chunks_to_send; i++) {
        offset = i * OA_CHUNK_SIZE;
        chunk_rx = (struct oa_rx_chunk_t*) &oa->rx_data[offset];
        chunk_rx->footer.bits = oa_convert_rx_chunk_footer(chunk_rx->footer.value);
    
        oa->tx_credits = chunk_rx->footer.bits.transmit_credits;
        oa->rx_available = chunk_rx->footer.bits.receive_chunks_available;

        if(chunk_rx->footer.bits.data_valid ){
            ret = oa_process_received_chunk(oa, chunk_rx);
            if(ret < 0)
                return ret;
        }
    }

    if(oa->cb_tx_finished)
        oa->cb_tx_finished(oa->cb_tx_finished_context);
    if(repeat)
        wake_up_process(oa->spi_thread);

    return 0;
}

static int oa_spi_transfer_thread(void *pv)
{
    struct open_alliance *oa = pv;
    int ret;
    bool val;

    while (!kthread_should_stop()) {
        schedule_timeout_interruptible(msecs_to_jiffies(5000) + 1);

        mutex_lock(&oa->int_lock);
        val = oa->int_received;
        oa->int_received = false;
        mutex_unlock(&oa->int_lock);

        mutex_lock(&oa->spi_lock);
        while (oa->rx_available || kfifo_len(&oa->tx_chunks_fifo) || val) {
            ret = oa_transfer_chunks(oa, val);
            if(ret < 0)
                OA_DEBUG("OA: transfer error %d", ret);
            
            val = false;
        }
        mutex_unlock(&oa->spi_lock);
    }

    return 0;
}

struct open_alliance *oa_init(struct spi_device *spidev, struct net_device *netdev){
    struct open_alliance *oa;
    int ret;

    oa = kzalloc(sizeof(*oa), GFP_KERNEL);
    if (!oa)
        return ERR_PTR(-ENOMEM);

    oa->spidev = spidev;
    oa->netdev = netdev;
    oa->spi_thread = kthread_create(oa_spi_transfer_thread, oa, "open-alliance-spi-thread");
    if (IS_ERR(oa->spi_thread))
        return ERR_CAST(oa->spi_thread);

    ret = kfifo_alloc(&oa->tx_chunks_fifo, OA_FIFO_CHUNK_NUM, GFP_KERNEL);
    if (ret < 0) {
        kfree(oa);
        return ERR_PTR(ret);
    }

    mutex_init(&oa->spi_lock);
    mutex_init(&oa->int_lock);
    spin_lock_init(&oa->tx_lock);

    return oa;
}

int oa_delete(struct open_alliance *oa){
    kfifo_free(&oa->tx_chunks_fifo);
    kfree(oa);
    return 0;
}

int oa_net_open(struct open_alliance *oa, int (*phy_config)(void *), void* context){
    u32 val;
    int ret;

    ret = oa_read_reg(oa, OA_REG_CONFIG0_MMS, OA_REG_CONFIG0, &val);
    if(ret < 0)
        return ret;

    if(val & OA_REG_CONFIG0_SYNC){
        ret = oa_write_reg(oa, OA_REG_RESET_MMS, OA_REG_RESET, OA_REG_RESET_RESET);
        if(ret < 0)
            return ret;

        mdelay(OA_RESET_DELAY_MS);
    }

    ret = oa_write_reg(oa, OA_REG_STATUS0_MMS, OA_REG_STATUS0, OA_REG_STATUS0_CLEAR);
    if(ret < 0)
        return ret;
    // lower level phy config
    ret = phy_config(context);
    if(ret < 0)
        return ret;

    val = OA_REG_CONFIG0_ZARFE | OA_REG_CONFIG0_CSARFE | OA_REG_CONFIG0_SYNC;
    val |= FIELD_PREP(OA_REG_CONFIG0_CPS, 0x6);
    ret = oa_write_reg(oa, OA_REG_CONFIG0_MMS, OA_REG_CONFIG0, val);

    oa->config_done = true;
    // get tx credits
    oa->int_received = true;
    wake_up_process(oa->spi_thread);
    return ret;
}

int oa_net_stop(struct open_alliance *oa){
    int ret;

    ret = kthread_stop(oa->spi_thread);
    if(ret < 0)
        return ret;

    oa->tx_credits = 0;
    oa->rx_available = 0;
    oa->frame_assembly_idx = 0;
    oa->frame_assembly_err = false;
    oa->config_done = false;

    spin_lock(&oa->tx_lock);
    kfifo_reset(&oa->tx_chunks_fifo);
    spin_unlock(&oa->tx_lock);

    return 0;
}

int oa_write_reg(struct open_alliance *oa, u8 mms, u16 addr, u32 val)
{
    struct oa_tx_control_t *ctrl_tx = (struct oa_tx_control_t*) oa->tx_data;
    struct oa_rx_control_t *ctrl_rx = (struct oa_rx_control_t*) oa->rx_data;
    struct spi_transfer t = {0};
    int ret;

    mutex_lock(&oa->spi_lock);
    ctrl_tx->header.value = 0;
    ctrl_tx->header.bits.write_not_read = 1;
    ctrl_tx->header.bits.memory_map_selector = mms;
    ctrl_tx->header.bits.address = addr;
    ctrl_tx->header.bits.parity = oa_calc_parity(ctrl_tx->header.value);
    ctrl_tx->header.value = oa_convert_control_header(ctrl_tx->header.bits);
    ctrl_tx->reg_val = cpu_to_be32(val);

    t.rx_buf = oa->rx_data;
    t.tx_buf = oa->tx_data;
    t.len = OA_CHUNK_CONTROL_SIZE;
    ret = spi_sync_transfer(oa->spidev, &t, 1);
    if (ret < 0) {
        mutex_unlock(&oa->spi_lock);
        return ret;
    }

    if(be32_to_cpu(ctrl_rx->reg_val) != val)
        ret = -EIO;
    mutex_unlock(&oa->spi_lock);
    return ret;
}

int oa_read_reg(struct open_alliance *oa, u8 mms, u16 addr,
                u32 *val){
    struct oa_tx_control_t *ctrl_tx = (struct oa_tx_control_t*) oa->tx_data;
    struct oa_rx_control_t *ctrl_rx = (struct oa_rx_control_t*) oa->rx_data;
    struct spi_transfer t = {0};
    int ret;

    mutex_lock(&oa->spi_lock);
    ctrl_tx->header.value = 0;
    ctrl_tx->header.bits.memory_map_selector = mms;
    ctrl_tx->header.bits.address = addr;
    ctrl_tx->header.bits.parity = oa_calc_parity(ctrl_tx->header.value);
    ctrl_tx->header.value = oa_convert_control_header(ctrl_tx->header.bits);
    ctrl_tx->reg_val = cpu_to_be32(val);

    t.rx_buf = oa->rx_data;
    t.tx_buf = oa->tx_data;
    t.len = OA_CHUNK_CONTROL_SIZE;

    ret = spi_sync_transfer(oa->spidev, &t, 1);
    if (ret < 0) {
        mutex_unlock(&oa->spi_lock);
        return ret;
    }

    *val = be32_to_cpu (ctrl_rx->reg_val);
    mutex_unlock(&oa->spi_lock);
    return 0;
}

int oa_reg_set_bits(struct open_alliance *oa, u8 mms, u16 addr,
                    u32 val, u32 mask){
    int ret;
    u32 read_val;

    ret = oa_read_reg(oa, mms, addr, &read_val);
    if(ret<0)
        return ret;

    set_mask_bits(&read_val, mask, val);
    return oa_write_reg(oa, mms, addr, read_val);
}

int oa_write_space(struct open_alliance *oa){
    return (OA_FIFO_CHUNK_NUM - kfifo_len(&oa->tx_chunks_fifo)) * OA_CHUNK_DATA_SIZE;
}

int oa_write(struct open_alliance *oa, struct sk_buff *txb){
    struct oa_tx_chunk_t chunk = {};
    u32 chunks_num;
    u32 len = txb->len;
    u32 last_chunk_len = len % OA_CHUNK_DATA_SIZE;
    int i;
    int ret;

    if(len > oa_write_space(oa))
        return -ENOMEM;

    chunks_num = len / OA_CHUNK_DATA_SIZE;
    if (last_chunk_len != 0)
        chunks_num += 1;

    for(i = 0; i < chunks_num; i++){
        chunk.header.value = 0;
        chunk.header.bits.data_not_control = 1;
        chunk.header.bits.data_valid = 1;
        chunk.header.bits.start_valid = !i;
        chunk.header.bits.end_valid = ((i + 1) == chunks_num);

        if((i+1) == chunks_num){
            chunk.header.bits.end_byte_offset = last_chunk_len - 1;
            memcpy(chunk.data, txb->data + (i * OA_CHUNK_DATA_SIZE), last_chunk_len);
        }else{
            memcpy(chunk.data, txb->data + (i * OA_CHUNK_DATA_SIZE), OA_CHUNK_DATA_SIZE);
        }

        ret = kfifo_in_spinlocked(&oa->tx_chunks_fifo, &chunk, 1, &oa->tx_lock);
        if (!ret)
            return -ENOMEM;
    }

    wake_up_process(oa->spi_thread);
    return 0;
}

int oa_process_interrupt(struct open_alliance *oa){
    OA_DEBUG("OA: interrupt received")
    if(oa->config_done) {
        mutex_lock(&oa->int_lock);
        oa->int_received = true;
        mutex_unlock(&oa->int_lock);

        wake_up_process(oa->spi_thread);
    }
    return 0;
}

void oa_set_callback_tx_finished(struct open_alliance* oa, void (*callback)(void*), void* context){
    oa->cb_tx_finished = callback;
    oa->cb_tx_finished_context = context;
}