
# INSTALL

This section describes the current install policy.

1. Create `slurm` user and group.

2. Build and install slurm:
```
sudo ./slurmviz-deployer all
```
  Please note that the slurmviz-deployer will assume the spack installation to
  reside under `/opt/${INSTALL_TYPE}`. This makes it easier to build, install
  and operate several installations at the same time.
  * `/opt/slurm-skretch` (`INSTALL_TYPE="slurm-skretch"`)
  * `/opt/slurm-skretch-testing` (`INSTALL_TYPE="slurm-skretch-testing"`)

3. Checkout the config-repo into `/opt/${INSTALL_TYPE}/deployed/etc`.

4. Symlink the service files found in the `config-repo` into systemd. The service files are parametrized to support several parallel deployments, as well.
```
for service in slurm{,ctl,db}d; do
  ln -vs ${INSTALL_PREFIX}/etc/${service}@.service /etc/systemd/system/${service}@${INSTALL_TYPE}.service
done
```

5. Start `slurm{db,ctl,}d@${INSTALL_TYPE}`-services!

