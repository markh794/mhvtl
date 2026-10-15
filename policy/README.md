# mhVTL mandatory-access-control policy

Optional AppArmor and SELinux confinement for the mhVTL daemons
(`vtltape`, `vtllibrary`). Both ship **non-enforcing** so that enabling them
cannot break a working deployment; an administrator reviews what the daemons
actually do and then promotes the policy to enforcing.

The policy targets the FHS layout: daemons in `/usr/libexec/mhvtl`, media in
`/var/lib/mhvtl`, the runtime socket dir in `/run/mhvtl`, lock files in
`/run/lock/mhvtl`, and the emulated SCSI nodes at `/dev/mhvtl<n>`.

## AppArmor (`apparmor/`)

Profiles for the two daemons, shipped in **complain** mode (`flags=(complain)`):
AppArmor logs accesses outside the profile but allows them.

```sh
sudo install -m 644 apparmor/usr.libexec.mhvtl.* /etc/apparmor.d/
sudo apparmor_parser -r /etc/apparmor.d/usr.libexec.mhvtl.vtltape \
                        /etc/apparmor.d/usr.libexec.mhvtl.vtllibrary
# exercise the daemons, review complaints:
sudo dmesg | grep apparmor   # or: sudo aa-logprof
# when the profile is complete, drop `flags=(complain)` and reload to enforce.
```

## SELinux (`selinux/`)

A reference-policy module defining an `mhvtl_t` domain, shipped as a
**permissive** domain (`permissive mhvtl_t;` in `mhvtl.te`): SELinux logs the
domain's AVCs without denying them.

```sh
make -C selinux                 # needs selinux-policy-devel -> mhvtl.pp
sudo semodule -i selinux/mhvtl.pp
sudo restorecon -R -v /usr/libexec/mhvtl /var/lib/mhvtl /run/mhvtl /run/lock/mhvtl
# exercise the daemons, review AVCs (ausearch -m avc -c vtltape), refine
# selinux/mhvtl.te, then remove the `permissive` line and reload to enforce.
```

## Status

These profiles cover the daemons' known behaviour (device-node creation,
SysV IPC, AF_UNIX sockets, media and config access) but have **not** been
validated in enforcing mode on every configuration. They are intentionally
non-enforcing until a deployment confirms coverage.
