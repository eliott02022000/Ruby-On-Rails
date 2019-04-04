require 'pathname'
require 'minitest/autorun'
require 'mocha/setup'

require 'debugger'

Dir.glob(File.expand_path("../support/*.rb", __FILE__)).each { |f| require f }

Debugger::Command.settings[:debuggertesting] = true
