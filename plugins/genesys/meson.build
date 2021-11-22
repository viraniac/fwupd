if get_option('gusb')
cargs = ['-DG_LOG_DOMAIN="FuPluginGenesys"']

install_data([
  'genesys.quirk',
  ],
  install_dir: join_paths(datadir, 'fwupd', 'quirks.d')
)

shared_module('fu_plugin_genesys',
  fu_hash,
  sources : [
    'fu-genesys-scaler-firmware.c',
    'fu-genesys-usbhub-firmware.c',
    'fu-genesys-scaler.c',
    'fu-genesys-usbhub.c',
    'fu-plugin-genesys.c',
  ],
  include_directories : [
    root_incdir,
    fwupd_incdir,
    fwupdplugin_incdir,
  ],
  install : true,
  install_dir: plugin_dir,
  link_with : [
    fwupd,
    fwupdplugin,
  ],
  c_args : cargs,
  dependencies : [
    plugin_deps,
  ],
)
endif
