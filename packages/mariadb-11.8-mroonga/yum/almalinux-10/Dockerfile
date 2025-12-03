ARG FROM=almalinux:10
FROM ${FROM}

ARG DEBUG

RUN \
  quiet=$([ "${DEBUG}" = "yes" ] || echo "--quiet") && \
  almalinux_version=$(cut -d: -f5 /etc/system-release-cpe | cut -d. -f1) && \
  dnf update -y ${quiet} && \
  { \
    echo "[mariadb]"; \
    echo "name = MariaDB"; \
    echo "baseurl = https://yum.mariadb.org/11.8/rhel/${almalinux_version}/x86_64/"; \
    echo "gpgkey = https://yum.mariadb.org/RPM-GPG-KEY-MariaDB"; \
    echo "gpgcheck = 1"; \
  } | tee /etc/yum.repos.d/MariaDB.repo && \
  dnf install -y \
    https://packages.apache.org/artifactory/arrow/almalinux/${almalinux_version}/apache-arrow-release-latest.rpm \
    https://packages.groonga.org/almalinux/${almalinux_version}/groonga-release-latest.noarch.rpm && \
  dnf groupinstall -y ${quiet} "Development Tools" && \
  dnf install --enablerepo=crb -y ${quiet} \
    'dnf-command(builddep)' \
    'dnf-command(download)' && \
  dnf builddep --enablerepo=crb -y ${quiet} MariaDB && \
  dnf download -y ${quiet} --source MariaDB && \
  dnf install --enablerepo=crb -y ${quiet} \
    MariaDB-devel \
    ccache \
    gcc-c++ \
    groonga-devel \
    groonga-normalizer-mysql-devel \
    intltool \
    libtool \
    make \
    pkgconfig \
    sudo \
    wget \
    which && \
  dnf clean ${quiet} all
