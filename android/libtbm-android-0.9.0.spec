Name:           libtbm-android
Version:        0.9.0
Release:        1
License:        MIT
Summary:        Tizen Buffer Manager - android backend
Group:          System/Libraries
Source0:        %{name}-%{version}.tar.gz

%description
Descriptionion: Tizen Buffer manager backend module for android.

%prep
# our %{name}-%{version}.tar.gz archive hasn't top-level directory,
# so we create it
%setup -q -c %{name}-%{version}

%build
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
./autogen.sh --build=x86_64-unknown-linux-gnu \
	     --host=arm-linux-androideabi \
	     --disable-static \
             CFLAGS="${CFLAGS} -Wall -Werror" \
             LDFLAGS="${LDFLAGS} -Wl,--hash-style=both -Wl,--as-needed"

make %{?_smp_mflags}

%install
rm -rf %{buildroot}

%make_install
cd %{buildroot}/usr/local/lib/bufmgr/ && ln -s libtbm_android.so libtbm_default.so

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root,-)
/usr/local/*
