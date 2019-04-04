if RUBY_VERSION < "2.0"
  STDERR.print("Ruby version is too old\n")
  exit(1)
end

require 'mkmf'
# $optflags = '-O0'
CONFIG['optflags'] = '-O0'


create_makefile('ruby_debug')
