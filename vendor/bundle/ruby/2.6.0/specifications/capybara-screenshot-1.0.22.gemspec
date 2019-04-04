# -*- encoding: utf-8 -*-
# stub: capybara-screenshot 1.0.22 ruby lib

Gem::Specification.new do |s|
  s.name = "capybara-screenshot".freeze
  s.version = "1.0.22"

  s.required_rubygems_version = Gem::Requirement.new(">= 0".freeze) if s.respond_to? :required_rubygems_version=
  s.require_paths = ["lib".freeze]
  s.authors = ["Matthew O'Riordan".freeze]
  s.date = "2018-10-21"
  s.description = "When a Cucumber step fails, it is useful to create a screenshot image and HTML file of the current page".freeze
  s.email = ["matthew.oriordan@gmail.com".freeze]
  s.homepage = "http://github.com/mattheworiordan/capybara-screenshot".freeze
  s.licenses = ["MIT".freeze]
  s.rubygems_version = "3.0.3".freeze
  s.summary = "Automatically create snapshots when Cucumber steps fail with Capybara and Rails".freeze

  s.installed_by_version = "3.0.3" if s.respond_to? :installed_by_version

  if s.respond_to? :specification_version then
    s.specification_version = 4

    if Gem::Version.new(Gem::VERSION) >= Gem::Version.new('1.2.0') then
      s.add_runtime_dependency(%q<capybara>.freeze, [">= 1.0", "< 4"])
      s.add_runtime_dependency(%q<launchy>.freeze, [">= 0"])
      s.add_development_dependency(%q<rspec>.freeze, [">= 0"])
      s.add_development_dependency(%q<timecop>.freeze, [">= 0"])
      s.add_development_dependency(%q<cucumber>.freeze, [">= 0"])
      s.add_development_dependency(%q<aruba>.freeze, [">= 0"])
      s.add_development_dependency(%q<sinatra>.freeze, [">= 0"])
      s.add_development_dependency(%q<test-unit>.freeze, [">= 0"])
      s.add_development_dependency(%q<spinach>.freeze, [">= 0"])
      s.add_development_dependency(%q<minitest>.freeze, [">= 0"])
      s.add_development_dependency(%q<aws-sdk-s3>.freeze, [">= 0"])
    else
      s.add_dependency(%q<capybara>.freeze, [">= 1.0", "< 4"])
      s.add_dependency(%q<launchy>.freeze, [">= 0"])
      s.add_dependency(%q<rspec>.freeze, [">= 0"])
      s.add_dependency(%q<timecop>.freeze, [">= 0"])
      s.add_dependency(%q<cucumber>.freeze, [">= 0"])
      s.add_dependency(%q<aruba>.freeze, [">= 0"])
      s.add_dependency(%q<sinatra>.freeze, [">= 0"])
      s.add_dependency(%q<test-unit>.freeze, [">= 0"])
      s.add_dependency(%q<spinach>.freeze, [">= 0"])
      s.add_dependency(%q<minitest>.freeze, [">= 0"])
      s.add_dependency(%q<aws-sdk-s3>.freeze, [">= 0"])
    end
  else
    s.add_dependency(%q<capybara>.freeze, [">= 1.0", "< 4"])
    s.add_dependency(%q<launchy>.freeze, [">= 0"])
    s.add_dependency(%q<rspec>.freeze, [">= 0"])
    s.add_dependency(%q<timecop>.freeze, [">= 0"])
    s.add_dependency(%q<cucumber>.freeze, [">= 0"])
    s.add_dependency(%q<aruba>.freeze, [">= 0"])
    s.add_dependency(%q<sinatra>.freeze, [">= 0"])
    s.add_dependency(%q<test-unit>.freeze, [">= 0"])
    s.add_dependency(%q<spinach>.freeze, [">= 0"])
    s.add_dependency(%q<minitest>.freeze, [">= 0"])
    s.add_dependency(%q<aws-sdk-s3>.freeze, [">= 0"])
  end
end
