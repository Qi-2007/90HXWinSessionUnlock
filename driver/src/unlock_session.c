/* Separate, temporary PCI function-driver build. NVIDIA binding is restored
 * by user mode only after the post-START IOCTL has closed DMA ownership. */
#define CMP_UNLOCK_SESSION
#include "lower_filter.c"
