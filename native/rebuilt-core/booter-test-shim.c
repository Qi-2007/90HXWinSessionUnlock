/* Research-only wrappers. Never link this into the shipped core. */
#include "core.h"
int backend_booter_target(gpu *,u64,boot_observation *,u32,u32,u32,u32);
int rebuilt_booter_test(gpu *g,u64 meta,boot_observation *o,u32 target,u64 wpr) {
 return rebuilt_booter_target(g,meta,o,target,(u32)wpr,(u32)(wpr>>32),0);
}
int reference_booter_test(gpu *g,u64 meta,boot_observation *o,u32 target,u64 wpr) {
 return backend_booter_target(g,meta,o,target,(u32)wpr,(u32)(wpr>>32),0);
}
