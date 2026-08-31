/* The gem's door for mruby's loader, and nothing else: slipstream has
 * no Ruby surface - it stands UNDER liburing, and the ruby-facing ring
 * API is mruby-io-uring's. These two exist because every mrbgem with
 * sources must answer them. */
#include <mruby.h>

void mrb_mruby_slipstreamio_gem_init(mrb_state *mrb) { (void) mrb; }

void mrb_mruby_slipstreamio_gem_final(mrb_state *mrb) { (void) mrb; }
