#ifndef __OPEN_ALLIANCE_H
#define __OPEN_ALLIANCE_H

#include <linux/cache.h>
#include <linux/kernel.h>
#include <linux/kfifo.h>
#include <linux/netdevice.h>
#include <linux/spi/spi.h>
#include <linux/etherdevice.h>

#include <asm/unaligned.h>

#define OA_REG_RESET                0x3
#define OA_REG_RESET_MMS            0x0
#define OA_REG_RESET_RESET          BIT(0)

#define OA_REG_CONFIG0              0x4
#define OA_REG_CONFIG0_MMS          0x0
#define OA_REG_CONFIG0_SYNC         BIT(15)
#define OA_REG_CONFIG0_CSARFE       BIT(13)
#define OA_REG_CONFIG0_ZARFE        BIT(12)
#define OA_REG_CONFIG0_TXCTRESH     GENMASK(11,10)
#define OA_REG_CONFIG0_CPS          GENMASK(2,0)

#define OA_REG_STATUS0             0x8
#define OA_REG_STATUS0_MMS         0x0
#define OA_REG_STATUS0_CLEAR       0xFFF

/* Transmit/Receiver Data common bitfields */
#define OA_FIELD_DV	                BIT(21)
#define OA_FIELD_SV	                BIT(20)
#define OA_FIELD_SWO	            GENMASK(19, 16)
#define OA_FIELD_EV	                BIT(14)
#define OA_FIELD_EBO	            GENMASK(13, 8)
#define OA_FIELD_PARITY		        BIT(0)

/* Transmit Data Header bitfields */
#define OA_FIELD_DNC	            BIT(31)
#define OA_FIELD_SEQ	            BIT(30)
#define OA_FIELD_NORX	            BIT(29)
#define OA_FIELD_TSC	            GENMASK(7, 6)

/* Receive Data Footer bitfields */
#define OA_FIELD_EXST	BIT(31)
#define OA_FIELD_HDRB	BIT(30)
#define OA_FIELD_SYNC	BIT(29)
#define OA_FIELD_RCA	GENMASK(28, 24)
#define OA_FIELD_FD	BIT(15)
#define OA_FIELD_RTSA	BIT(7)
#define OA_FIELD_RTSP	BIT(6)
#define OA_FIELD_TXC	GENMASK(5, 1)

/* Control Command Header bitfields */
#define OA_FIELD_WNR	BIT(29)
#define OA_FIELD_AID	BIT(28)
#define OA_FIELD_MMS	GENMASK(27, 24)
#define OA_FIELD_ADDR	GENMASK(23, 8)
#define OA_FIELD_LEN	GENMASK(7, 1)

#define OA_RESET_DELAY_MS           50

#define OA_CHUNK_HEADER_SIZE        4
#define OA_CHUNK_DATA_SIZE	        64
#define OA_CHUNK_CONTROL_SIZE       12
#define OA_CHUNK_SIZE               (OA_CHUNK_HEADER_SIZE + OA_CHUNK_DATA_SIZE)

#define OA_FIFO_CHUNK_NUM           1024 // number of chunks to buffer

#define OA_TRX_BUFFER_CHUNKS        32ul
#define OA_TRX_BUFFER_SIZE          (OA_TRX_BUFFER_CHUNKS * OA_CHUNK_SIZE) // max possible size of spi write (until phy memory is filled)

#define OA_FRAME_ASSEMBLY_CHUNKS    32ul
#define OA_FRAME_ASSEMBLY_SIZE      (OA_FRAME_ASSEMBLY_CHUNKS * OA_CHUNK_DATA_SIZE)

#define OA_DEBUG(...) printk(KERN_INFO __VA_ARGS__);

struct oa_control_header_t {
    u32 data_not_control : 1;
    u32 received_header_bad : 1;
    u32 write_not_read : 1;
    u32 address_increment_disable : 1;
    u32 memory_map_selector : 4;
    u32 address : 16;
    u32 length : 7;
    u32 parity : 1;
};

struct oa_tx_chunk_header_t{
    u32 data_not_control : 1;
    u32 data_chunk_sequence : 1;
    u32 no_receive : 1;
    u32  : 5;
    u32  : 2;
    u32 data_valid : 1;
    u32 start_valid : 1 ;
    u32 start_word_offset : 4;
    u32  : 1;
    u32 end_valid : 1;
    u32 end_byte_offset : 6;
    u32 transmit_frame_timestamp_capture : 2;
    u32  : 5;
    u32 parity : 1;
};

struct oa_rx_chunk_footer_t{
    u32 extended_status : 1;
    u32 received_header_bad : 1;
    u32 configuration_synchronized : 1;
    u32 receive_chunks_available : 5;
    u32  : 2;
    u32 data_valid : 1;
    u32 start_valid : 1;
    u32 start_word_offset : 4;
    u32 frame_drop : 1;
    u32 end_valid : 1;
    u32 end_byte_offset : 6;
    u32 receive_frame_timestamp_added : 1;
    u32 receive_frame_timestamp_parity : 1;
    u32 transmit_credits : 5;
    u32 parity : 1;
};

struct oa_tx_control_t{
    union {
        struct oa_control_header_t bits;
        u32 value;
    } header;
    u32 reg_val;
    u32 ignore;
};

struct oa_rx_control_t {
    u32 ignore;
    union {
        struct oa_control_header_t bits;
        u32 value;
    } header;
    u32 reg_val;
};

struct oa_rx_chunk_t{
    u8 data [OA_CHUNK_DATA_SIZE];
    union {
        struct oa_rx_chunk_footer_t bits;
        u32 value;
    }footer;
};

struct oa_tx_chunk_t{
    union {
        struct oa_tx_chunk_header_t bits;
        u32 value;
    }header;
    u8 data [OA_CHUNK_DATA_SIZE];
};

struct open_alliance {
    struct mutex		    spi_lock; // protect spi writes
    struct mutex            int_lock; // protect int signal
    spinlock_t		        tx_lock;  // protect tx chunks fifo
    struct spi_device*      spidev;
    struct net_device*      netdev;
    struct task_struct*     spi_thread;
    u32			            tx_credits;
    u32			            rx_available;
    bool                    int_received;
    bool                    config_done;
    u32                     frame_assembly_idx;
    bool                    frame_assembly_err;
    void                    (*cb_tx_finished)(void*);
    void*                   cb_tx_finished_context;
    DECLARE_KFIFO_PTR(tx_chunks_fifo, struct oa_tx_chunk_t);
    u8                      frame_assembly [OA_FRAME_ASSEMBLY_SIZE];
    u8			            tx_data [OA_TRX_BUFFER_SIZE];
    u8			            rx_data [OA_TRX_BUFFER_SIZE] ____cacheline_aligned;
};

struct open_alliance *oa_init(struct spi_device *spidev, struct net_device *netdev);
int oa_delete(struct open_alliance *oa);

int oa_net_open(struct open_alliance *oa, int (*phy_config)(void *), void* context);
int oa_net_stop(struct open_alliance *oa);

int oa_write_reg(struct open_alliance *oa, u8 mms, u16 addr,
                            u32 val);
int oa_read_reg(struct open_alliance *oa, u8 mms, u16 addr,
                           u32 *val);
int oa_reg_set_bits(struct open_alliance *oa, u8 mms, u16 addr,
                               u32 val, u32 mask);

int oa_write(struct open_alliance *oa, struct sk_buff *txb);
int oa_write_space(struct open_alliance *oa);
int oa_process_interrupt(struct open_alliance *oa);

void oa_set_callback_tx_finished(struct open_alliance* oa, void (*callback)(void*), void* context);
#endif /* __OPEN_ALLIANCE_H */
