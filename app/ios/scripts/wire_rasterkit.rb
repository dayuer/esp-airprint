# frozen_string_literal: true
#
# 把 RasterKit 的 ObjC++ 实现和 shared/urf 的 C++ 源文件加进 Xcode target。
#
# 可以重复跑：每次先移除同名引用再加回去。
#
#   ruby ios/scripts/wire_rasterkit.rb
#
# 为什么不复制 shared/urf 的源文件进 ios/：复制出来的那份就再也跑不了
# shared/urf 的单测了，而那 35 个单测是这个项目里唯一能在不烧纸的前提下
# 验证光栅正确性的手段。
require 'xcodeproj'

proj_path = File.expand_path('../AirPrint.xcodeproj', __dir__)
proj = Xcodeproj::Project.open(proj_path)
target = proj.targets.find { |t| t.name == 'AirPrint' }
abort '找不到 AirPrint target' unless target

def reset_group(proj, name, path)
  g = proj.main_group.find_subpath(name, true)
  g.set_source_tree('SOURCE_ROOT')
  g.set_path(path)
  g
end

def add_source(group, target, filename)
  group.files.select { |f| f.display_name == filename }.each(&:remove_from_project)
  ref = group.new_reference(filename)
  target.source_build_phase.add_file_reference(ref, true)
  ref
end

rk = reset_group(proj, 'RasterKit', 'RasterKit')
add_source(rk, target, 'RasterKit.mm')
rk.files.select { |f| f.display_name == 'RasterKit.h' }.each(&:remove_from_project)
rk.new_reference('RasterKit.h')

urf = reset_group(proj, 'SharedURF', '../shared/urf/src')
%w[packbits.cpp writer.cpp validate.cpp].each { |n| add_source(urf, target, n) }

target.build_configurations.each do |c|
  s = c.build_settings
  paths = Array(s['HEADER_SEARCH_PATHS'] || ['$(inherited)'])
  paths |= ['$(SRCROOT)/../shared/urf/include', '$(SRCROOT)/RasterKit']
  s['HEADER_SEARCH_PATHS'] = paths
  s['CLANG_CXX_LANGUAGE_STANDARD'] = 'c++20'
end

proj.save
puts '已加入 target: RasterKit.mm + shared/urf 的三个 .cpp'
