# Device build profiles

The default `prj.conf` builds the currently available `FING_20MM` ring. Use
one of the profile fragments when producing firmware for a specific physical
ring:

```sh
west build -p always -b nrf54v1/nrf54l15/cpuapp --sysbuild . -- \
  -DBOARD_ROOT=/Users/yuxindu/VSCode/nrf/nrf54_projects \
  -DEXTRA_CONF_FILE=configs/fing_20mm.conf
```

Replace `fing_20mm.conf` with `fing_18mm.conf` or `fing_22mm.conf` for the
other two rings. Each physical board must always be flashed with its matching
profile so its stable identity remains meaningful in exported study data.
