MRuby::Gem::Specification.new('mruby-slipstreamio') do |spec|
  spec.license = 'Apache-2.0'
  spec.author  = 'Hendrik Beskow'
  spec.summary = 'what liburing needs from underneath when the kernel refuses, or is not Linux'

  # src/*.c build as every gem's sources do: the syscall switch, the
  # engine core, and the backends the platform guards let through. They
  # read the CARRIED liburing's io_uring.h - mruby-io-uring installs its
  # liburing's headers into its include/, and every mrbgem's include/ is
  # on every other gem's search path, so nothing is configured here.
  #
  # The seam itself acts only when a liburing is built WITH it:
  # mruby-io-uring copies src/liburing_syscall.h over liburing's
  # src/syscall.h before running liburing's own build, and from then on
  # every one of liburing's twelve __sys_* calls lands in this gem's
  # objects first - kernel or engine decided at runtime, per process,
  # by asking the kernel.
  #
  # src/ is exported so a consumer that wants to KNOW which side answers
  # (webmachine's startup banner) may ask slipstream_syscall_uses_engine.
  # That is reporting, not steering - the decision stays slipstream's.
  spec.export_include_paths << "#{spec.dir}/src"

  # test/ holds the repo's own standalone proofs, driven by its Makefile
  # and the VM harnesses - none of it is an mruby test file.
  spec.test_rbfiles = []
  spec.test_objs = []

  # The engine is C11 - <threads.h>, timespec_get - and mruby's default
  # C dialect can be older; the gem states its own.
  spec.cc.flags << '-std=gnu11'

  # The circle with mruby-io-uring, stated: its liburing.a needs THIS
  # gem's symbols (that is the seam), and this gem's sources need ITS
  # installed liburing headers - the carried ABI. Dependencies only flow
  # one way in mruby, so the header direction is wired by path: the
  # sibling clone in repos/, or SLIPSTREAM_LIBURING_INC for a build that
  # keeps its liburing elsewhere.
  liburing_inc = ENV['SLIPSTREAM_LIBURING_INC'] ||
                 File.expand_path('../mruby-io_uring/include', spec.dir)
  [spec.cc, spec.cxx].each { |c| c.include_paths << liburing_inc }

  # Off Linux, liburing.h and the engine also need the shim headers -
  # shim/windows or shim/posix first, shim/common always. On Linux the
  # system provides all of it and the shims stay off the path.
  unless RUBY_PLATFORM.include?('linux')
    platform_dir = RUBY_PLATFORM.include?('mingw') || RUBY_PLATFORM.include?('mswin') ? 'windows' : 'posix'
    [spec.cc, spec.cxx].each do |c|
      c.include_paths << "#{spec.dir}/shim/#{platform_dir}"
      c.include_paths << "#{spec.dir}/shim/common"
    end
    spec.export_include_paths << "#{spec.dir}/shim/#{platform_dir}"
    spec.export_include_paths << "#{spec.dir}/shim/common"
  end
end
