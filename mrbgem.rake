MRuby::Gem::Specification.new('mruby-slipstreamio') do |spec|
  spec.license = 'Apache-2.0'
  spec.author  = 'Hendrik Beskow'
  spec.summary = "io_uring's model - the API and the engine behind it - on select(2)"

  # THE DECISION LIVES HERE, and only here. The header itself never
  # looks for the implementation it stands in for, and neither does any
  # consumer: they write `#include <liburing.h>` and call the
  # functions. This block is the packaging layer that decides what that
  # resolves to - the job libkqueue leaves to the distribution.
  #
  # mruby-io-uring is the dependency because it is the one that knows:
  # it runs liburing's own ./configure && make on this host and copies
  # the installed headers into its include/ when that works, or
  # degrades (URING_AVAILABLE = false) when it does not. Every mrbgem's
  # include/ is on every other mrbgem's search path, so "did it work"
  # is answerable by looking.
  spec.add_dependency 'mruby-io-uring'

  own = "#{spec.dir}/src/liburing.h"        # the implementation, always here
  installed = "#{spec.dir}/include/liburing.h"  # the copy, only when needed

  # Removed BEFORE looking, every build. Otherwise the search below
  # would find last build's own copy and conclude a real liburing is
  # present - and a host that gained liburing since would keep being
  # served the select implementation forever. The question has to be
  # asked against everyone else's include/, not ours.
  FileUtils.rm_f installed

  if spec.cc.search_header 'liburing.h'
    # A real liburing is on the path (mruby-io-uring built it). Nothing
    # to install and nothing to say: src/liburing.h stays where it is,
    # off every include path, and no consumer reaches it. Silence is
    # the correct output of a decision that changed nothing.
  else
    # No liburing anywhere. Put ours on the include path, so source
    # written against that API compiles and runs here unchanged.
    #
    # Build output, copied fresh every build - not tracked source, see
    # .gitignore.
    FileUtils.mkdir_p "#{spec.dir}/include"
    FileUtils.cp own, installed

    # LINKING, measured on the host that is building and not guessed at.
    # The implementation runs an engine thread and a small work pool, so
    # it needs C11 <threads.h>. Two questions, and only the first of
    # them is asked here:
    #
    #   IS THERE ONE? glibc has had thrd_* since 2.34, musl has them,
    #   MSVC since VS 2022 17.8. macOS has never shipped <threads.h> -
    #   that platform needs a small shim over pthreads and it is a named
    #   part of its task in TASKS.md, not something to paper over here.
    #
    #   WHAT MUST BE LINKED? On this build host: nothing. thrd_create
    #   lives in libc.so.6 itself (glibc 2.39), and a program using it
    #   links with no extra library - checked, not assumed. Hosts that
    #   kept C11 threads in a separate library are where
    #   spec.linker.libraries would gain 'pthread', and the way to know
    #   is to measure that host, which is what this comment is here to
    #   say. Nothing is linked on suspicion: a flag added "just in case"
    #   is a flag nobody can ever remove.
    unless spec.cc.search_header 'threads.h'
      warn 'mruby-slipstreamio: no <threads.h> on this host. The engine needs C11 threads; ' \
           'see TASKS.md (macOS ships none to this day and needs a thrd_ shim).'
    end

    warn 'mruby-slipstreamio: no liburing on this host -- <liburing.h> now resolves to ' \
         'slipstreamIO (the select(2) baseline: correct, NOT fast).'
  end
end
