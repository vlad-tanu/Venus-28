#include "TCA9548A.h"
#include <libpynq.h>

/* low-level write to the control register (pointer = 0x00) */
static int write_control(iic_index_t iic, uint8_t ctrl)
{
    return iic_write_register(iic, TCA9548A_I2C_ADDR, 0x00, &ctrl, 1);
}

int tca9548a_init(iic_index_t iic, tca9548a *mux)
{
    if (!mux) return 1;
    mux->iic_index       = iic;
    mux->current_channel = 0xFF;          /* none selected */
    return write_control(iic, 0x00);      /* disable all channels */
}

int tca9548a_destroy(tca9548a *mux)
{
    if (!mux) return 1;
    mux->current_channel = 0xFF;
    return write_control(mux->iic_index, 0x00);
}

int tca9548a_select_channel(tca9548a *mux, uint8_t channel)
{
    if (!mux || channel >= TCA9548A_CHANNEL_COUNT) return 1;
    uint8_t ctrl = 1u << channel;
    int err = write_control(mux->iic_index, ctrl);
    if (!err) mux->current_channel = channel;
    return err;
}
