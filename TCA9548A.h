#ifndef TCA9548A_H
#define TCA9548A_H

#include <stdint.h>
#include <libpynq.h>

/* Default 7-bit address when A2=A1=A0 = 0-0-0 */
#define TCA9548A_I2C_ADDR      0x70
#define TCA9548A_CHANNEL_COUNT 8

/* Handle for one TCA9548A device */
typedef struct tca9548a_mux {
    iic_index_t iic_index;     /* which I²C bus to use           */
    uint8_t     current_channel;  /* last-selected channel 0-7, 0xFF = none */
} tca9548a;

/* API – exactly as in the original file */
int tca9548a_init          (iic_index_t iic, tca9548a *mux);
int tca9548a_destroy       (tca9548a *mux);
int tca9548a_select_channel(tca9548a *mux, uint8_t channel);

#endif /* TCA9548A_H */
