#ifndef SPEED_H
#define SPEED_H

// 25MHZ
#define NSECS_PER_CLOCK  40

// Poweroff takes 5 ms = 5 million ns
#define POWEROFF_CLOCKS (5000000 / NSECS_PER_CLOCK)

// 19200 bps serial.
//
// However, System/161 doesn't actually manage to run at 25MHz; it gets a
// little over 1MHz on my devel box (PIII-550). This means that the effective
// output speed is only 768 bps. That's positively glacial.
//
// To avoid making everybody wait forever to see output scroll by,
// we introduce a factor SERIAL_FUDGE that speeds up serial I/O.
//
// I'm going to set it to 25, so the actual output speed I see is about 
// 19200 bps.
//

#define SERIAL_FUDGE   25
#define SERIAL_NSECS   (1000000000/((19200*(SERIAL_FUDGE))/10))

#endif /* SPEED_H */
