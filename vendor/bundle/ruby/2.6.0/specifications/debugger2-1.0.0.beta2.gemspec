# -*- encoding: utf-8 -*-
# stub: debugger2 1.0.0.beta2 ruby lib
# stub: ext/ruby_debug/extconf.rb

Gem::Specification.new do |s|
  s.name = "debugger2".freeze
  s.version = "1.0.0.beta2"

  s.required_rubygems_version = Gem::Requirement.new(">= 1.3.6".freeze) if s.respond_to? :required_rubygems_version=
  s.require_paths = ["lib".freeze]
  s.authors = ["Koichi Sasada".freeze, "Zachary Scott".freeze, "Kent Sibilev".freeze, "Mark Moseley".freeze, "Gabriel Horner".freeze]
  s.date = "2013-03-19"
  s.description = "debugger is a fast implementation of the standard Ruby debugger debug.rb.\nIt is implemented by utilizing a new Ruby C API hook. The core component\nprovides support that front-ends can build on. It provides breakpoint\nhandling, bindings for stack frames among other things.\n".freeze
  s.email = "ko1@atdot.net".freeze
  s.executables = ["rdebug".freeze]
  s.extensions = ["ext/ruby_debug/extconf.rb".freeze]
  s.extra_rdoc_files = ["README.md".freeze]
  s.files = ["README.md".freeze, "bin/rdebug".freeze, "ext/ruby_debug/extconf.rb".freeze]
  s.homepage = "https://github.com/ko1/debugger2".freeze
  s.licenses = ["BSD".freeze]
  s.rubygems_version = "3.0.1".freeze
  s.summary = "Fast Ruby debugger - base + cli".freeze

  s.installed_by_version = "3.0.1" if s.respond_to? :installed_by_version

  if s.respond_to? :specification_version then
    s.specification_version = 4

    if Gem::Version.new(Gem::VERSION) >= Gem::Version.new('1.2.0') then
      s.add_runtime_dependency(%q<columnize>.freeze, [">= 0.3.1"])
      s.add_runtime_dependency(%q<debugger-linecache>.freeze, ["~> 1.2.0"])
      s.add_development_dependency(%q<rake>.freeze, ["~> 0.9.2.2"])
      s.add_development_dependency(%q<rake-compiler>.freeze, ["~> 0.8.0"])
      s.add_development_dependency(%q<minitest>.freeze, ["~> 2.12.1"])
      s.add_development_dependency(%q<mocha>.freeze, ["~> 0.13.0"])
    else
      s.add_dependency(%q<columnize>.freeze, [">= 0.3.1"])
      s.add_dependency(%q<debugger-linecache>.freeze, ["~> 1.2.0"])
      s.add_dependency(%q<rake>.freeze, ["~> 0.9.2.2"])
      s.add_dependency(%q<rake-compiler>.freeze, ["~> 0.8.0"])
      s.add_dependency(%q<minitest>.freeze, ["~> 2.12.1"])
      s.add_dependency(%q<mocha>.freeze, ["~> 0.13.0"])
    end
  else
    s.add_dependency(%q<columnize>.freeze, [">= 0.3.1"])
    s.add_dependency(%q<debugger-linecache>.freeze, ["~> 1.2.0"])
    s.add_dependency(%q<rake>.freeze, ["~> 0.9.2.2"])
    s.add_dependency(%q<rake-compiler>.freeze, ["~> 0.8.0"])
    s.add_dependency(%q<minitest>.freeze, ["~> 2.12.1"])
    s.add_dependency(%q<mocha>.freeze, ["~> 0.13.0"])
  end
end
