ARG FROM=almalinux:8
FROM ${FROM}

ARG DEBUG

RUN \
  quiet=$([ "${DEBUG}" = "yes" ] || echo "--quiet") && \
  dnf update -y ${quiet} && \
  { \
    echo "[mariadb]"; \
    echo "name = MariaDB"; \
    echo "baseurl = https://yum.mariadb.org/11.4/rhel/8/x86_64/"; \
    echo "gpgkey = https://yum.mariadb.org/RPM-GPG-KEY-MariaDB"; \
    echo "gpgcheck = 1"; \
  } | tee /etc/yum.repos.d/MariaDB.repo && \
  dnf install -y \
    https://packages.apache.org/artifactory/arrow/almalinux/8/apache-arrow-release-latest.rpm \
    https://packages.groonga.org/almalinux/8/groonga-release-latest.noarch.rpm && \
  dnf groupinstall -y ${quiet} "Development Tools" && \
  dnf install --enablerepo=powertools -y ${quiet} \
    'dnf-command(builddep)' \
    'dnf-command(download)' && \
  dnf module --enablerepo=powertools -y ${quiet} enable mariadb-devel && \
  dnf builddep --enablerepo=powertools -y ${quiet} MariaDB && \
  dnf module --enablerepo=powertools -y ${quiet} disable mariadb-devel && \
  dnf module -y ${quiet} disable mariadb && \
  dnf module -y ${quiet} disable mysql && \
  dnf download -y ${quiet} --source MariaDB && \
  dnf install --enablerepo=powertools -y ${quiet} \
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
