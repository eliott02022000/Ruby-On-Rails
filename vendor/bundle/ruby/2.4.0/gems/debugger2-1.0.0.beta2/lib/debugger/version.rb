module Debugger
  # TODO: remove version from C ext
  send :remove_const, :VERSION if const_defined? :VERSION
  VERSION = '1.0.0.beta2'
end
