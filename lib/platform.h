#ifndef platform_h
#define platform_h

// returns a number that increments in seconds for comparison (epoch or just since boot)
unsigned long platform_seconds();

unsigned short platform_short(unsigned short x);
  
#endif