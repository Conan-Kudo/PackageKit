
add_languages('cpp', native: false)

dnf5_dep = dependency('libdnf5', version: '>=5.2.17.0')
libdnf5_version = dnf5_dep.version().split('.')

shared_module(
  'pk_backend_dnf5',
  'pk-backend-dnf5.cpp',
  'dnf5-backend-utils.cpp',
  'dnf5-backend-thread.cpp',
  'dnf5-backend-vendor-@0@.cpp'.format(get_option('dnf_vendor')),
  dependencies: [
    dnf5_dep,
    packagekit_glib2_dep,
  ],
  cpp_args: [
    '-std=c++20',
    '-DLIBDNF5_VERSION_MAJOR=' + libdnf5_version[0],
    '-DLIBDNF5_VERSION_MINOR=' + libdnf5_version[1],
    '-DLIBDNF5_VERSION_PATCH=' + libdnf5_version[2],
    '-DG_LOG_DOMAIN="PackageKit-DNF5"',
  ],
  include_directories: packagekit_src_include,
  install: true,
  install_dir: pk_plugin_dir,
)

# Build rpm plugin for notifying PackageKit
rpm_dep = dependency('rpm', required: false)
dbus_dep = dependency('dbus-1', required: false)

if rpm_dep.found() and dbus_dep.found()
  rpm_plugindir = rpm_dep.get_variable(pkgconfig: 'rpmplugindir')
  rpm_macrosdir = rpm_dep.get_variable(pkgconfig: 'rpmhome') + '/macros.d'

  shared_module(
    'notify_packagekit',
    'notify_packagekit.c',
    dependencies: [
      rpm_dep,
      dbus_dep,
    ],
    install: true,
    install_dir: rpm_plugindir,
  )

  install_data(
    'macros.transaction_notify_packagekit',
    install_dir: rpm_macrosdir,
  )
endif
