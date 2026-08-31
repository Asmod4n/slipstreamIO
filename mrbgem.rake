MRuby::Gem::Specification.new('mruby-slipstreamio') do |spec|
  spec.license = 'Apache-2.0'
  spec.author  = 'Hendrik Beskow'
  spec.summary = 'liburing, carried and built with the seam - kernel or engine decided at runtime'

  # THE CARRIED LIBURING LIVES HERE. deps/liburing is the one liburing
  # of the whole ecosystem, pinned; this gem builds it WITH the seam -
  # src/liburing_syscall.h copied over its src/syscall.h before its own
  # configure runs - so every one of liburing's twelve __sys_* calls
  # lands in this gem's objects first: the kernel answers where io_uring
  # is allowed, the engine answers where it is not, decided at RUNTIME,
  # per process, by asking the kernel. Consumers depend on this gem and
  # write #include <liburing.h>; the installed headers ride on this
  # gem's exported include path.
  #
  # ENABLE_SHARED=0 and the library target only: a liburing.so cannot
  # link alone with the seam in it - the slipstream symbols live in the
  # consuming binary - and liburing's own test programs would each need
  # them too. The archive into the binary is the shape.
  liburing_src = "#{spec.dir}/deps/liburing"
  liburing_out = "#{spec.build_dir}/build"
  liburing_lib = "#{liburing_out}/lib/liburing.a"
  seam_marker = "#{liburing_out}/.slipstream-seam"
  unless File.file?(liburing_lib) && File.file?(seam_marker)
    FileUtils.cp "#{spec.dir}/src/liburing_syscall.h", "#{liburing_src}/src/syscall.h"
    FileUtils.cp "#{spec.dir}/src/liburing_arch_syscall.h", "#{liburing_src}/src/"
    FileUtils.cp "#{spec.dir}/src/slipstream_syscall.h", "#{liburing_src}/src/"
    command = "mkdir -p #{liburing_out} && cd #{liburing_src} && ./configure"
    if spec.cc.flags.any? { |entry| entry.is_a?(String) && entry.start_with?('-fsanitize=') }
      command << ' --enable-sanitizer'
    end
    command << " --prefix=\"#{liburing_out}\" --cc=\"#{spec.cc.command}\" --cxx=\"#{spec.cxx.command}\""
    command << ' && make -j$(nproc) -C src ENABLE_SHARED=0 && make install ENABLE_SHARED=0 && make -C src clean'
    built = false
    sh(command) { |ok, _status| built = ok }
    unless built
      abort 'mruby-slipstreamio: the carried liburing did not build (see above) - ' \
            'that is a broken build host, reported instead of served around.'
    end
    # The marker says WHICH archive this is: one whose syscalls go
    # through the seam. One from before the seam is rebuilt, not
    # trusted.
    FileUtils.touch(seam_marker)
  end
  spec.linker.flags_after_libraries << liburing_lib

  # The installed headers, not deps/liburing's source tree: compat.h and
  # io_uring_version.h exist only after configure ran, and what lands on
  # the path is exactly what this build was configured for. Exported, so
  # every dependent gem's #include <liburing.h> is this liburing.
  spec.export_include_paths << "#{liburing_out}/include"
  [spec.cc, spec.cxx].each { |c| c.include_paths << "#{liburing_out}/include" }

  # src/*.c build as every gem's sources do: the syscall switch, the
  # engine core, and the backends the platform guards let through.
  #
  # src/ is exported so a consumer that wants to KNOW which side answers
  # (webmachine's startup banner, mruby-io-uring's URING_AVAILABLE) may
  # ask slipstream_syscall_uses_engine. That is reporting, not steering
  # - the decision stays slipstream's.
  spec.export_include_paths << "#{spec.dir}/src"

  # The engine is C11 - <threads.h>, timespec_get - and mruby's default
  # C dialect can be older; the gem states its own.
  spec.cc.flags << '-std=gnu11'

  # test/ holds the repo's own standalone proofs, driven by its Makefile
  # and the VM harnesses - none of it is an mruby test file.
  spec.test_rbfiles = []
  spec.test_objs = []

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
