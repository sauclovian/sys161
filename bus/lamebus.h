
#ifndef LAMEBUS_H
#define LAMEBUS_H

/*
 * Info for a simulated device.
 */
struct lamebus_device_info {
   u_int32_t ldi_vendorid;
   u_int32_t ldi_deviceid;
   u_int32_t ldi_revision;
   void   *(*ldi_init)(int slot, int argc, char *argv[]);
   int     (*ldi_fetch)(void *, u_int32_t offset, u_int32_t *rt);
   int     (*ldi_store)(void *, u_int32_t offset, u_int32_t val);
   void    (*ldi_cleanup)(void *);
};

/*
 * Existing devices.
 */
extern const struct lamebus_device_info
   timer_device_info,
   disk_device_info,
   serial_device_info,
   screen_device_info,
   net_device_info,
   emufs_device_info,
   random_device_info;

/*
 * Interrupt management.
 */
extern u_int32_t bus_interrupts;

#define RAISE_IRQ(slot) (bus_interrupts |= (1<<(u_int32_t)(slot)))
#define LOWER_IRQ(slot) (bus_interrupts &= ~(1<<(u_int32_t)(slot)))
#define CHECK_IRQ(slot) ((bus_interrupts & (u_int32_t)(slot)) != 0)

#endif /* LAMEBUS_H */
