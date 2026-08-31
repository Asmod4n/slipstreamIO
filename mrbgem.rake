MRuby::Gem::Specification.new('mruby-slipstreamio') do |spec|
  spec.license = 'Apache-2.0'
  spec.author  = 'Hendrik Beskow'
  spec.summary = 'what liburing needs from underneath when the kernel refuses, or is not Linux'

  # src/*.c build like any gem's sources: the syscall switch and the
  # engine. They only act when a liburing is built WITH the seam -
  # test/with_liburing.sh shows the move (liburing's src/syscall.h
  # replaced by ours before its build runs). Carrying a liburing tree
  # here and running that build per consumer is the named next step in
  # TASKS.md ("Carrying liburing, and the packaging step"); until it
  # lands, mruby-io-uring's mrbgem.rake is the layer that builds
  # liburing, and it is the template for the carried one.
  #
  # Off Linux, add shim/windows or shim/posix plus shim/common to the
  # include paths of whatever compiles against liburing.h - that is what
  # lets liburing's own header compile there at all.
end
