#!/usr/bin/env python
# -*- coding: utf-8 -*-

import os, shutil
from conans import ConanFile, tools, CMake

class RecklessConan(ConanFile):
    name = 'reckless'
    license = 'Copyright 2015-2020 Mattias Flodin <git@codepentry.com>'
    url = 'https://github.com/mattiasflodin/reckless'
    description = """Reckless is an extremely low-latency, high-throughput logging library."""
    generators = 'cmake'
    settings = 'arch', 'cppstd', 'compiler', 'build_type'
    
    _build_subfolder = 'build'
    _source_subfolder = 'src'

    def configure(self):
        pass
    
    def source(self):
        tools.get('%s/archive/v%s.tar.gz' % (self.url, self.version))
        shutil.move('reckless-%s' % self.version, self._source_subfolder)

    def _configure_cmake(self):

        cmake = CMake(self)
        cmake.configure(build_folder=self._build_subfolder, source_folder=self._source_subfolder)
        return cmake

    def build(self):
        os.makedirs(self._build_subfolder)
        with tools.chdir(self._build_subfolder):
            cmake = self._configure_cmake()
            cmake.build()

    
    def package(self):
        
        self.copy('*.a', src='build', dst='lib', keep_path=False)
        self.copy('*.lib', src='build', dst='lib', keep_path=False)
        self.copy('*.hpp', src='src/reckless/include', dst='include', keep_path=True)

        if self.settings.build_type == 'Debug':
            self.copy('*.cpp', src='src/reckless/src', dst='src', keep_path=True)
            self.copy('*.pdb', src='build', dst='lib', keep_path=False)        

        self.copy('LICENSE.txt', src='src')

    def package_info(self):
        self.cpp_info.libs = ["reckless"]
        pass
