#
# spec file for package cups-autoconfig 
#
# Copyright (c) 2007 SUSE LINUX Products GmbH, Nuernberg, Germany.
# This file and all modifications and additions to the pristine
# package are under the same license as the package itself.
#
# Please submit bugfixes or comments via http://bugs.opensuse.org/
#

Name:           cups-autoconfig
Version:        0.1.0
Release:        1
Group:          System/Base
License:        GNU General Public License (GPL) - all versions
Summary:        A Utility to Auto-configure Printers. 
Autoreqprov:    on
BuildRoot:      %{_tmppath}/%{name}-%{version}-build
Requires:       cups >= 1.2 glib2 >= 2.8 hal dbus-1
BuildRequires:  glib2-devel >= 2.8 dbus-1-devel intltool hal cups-devel hal-devel
Source:         %{name}-%{version}.tar.gz

%description
This package contains a utility for auto-configuring printers.

Authors:
--------
    Chris Rivera <crivera@novell.com>

%prep
%setup

%build
export CFLAGS="$RPM_OPT_FLAGS"
%configure --with-slibdir=/%{_lib}

%install
make install DESTDIR=${RPM_BUILD_ROOT}

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root)
%dir %{_libdir}/cups-autoconfig
%{_libdir}/cups-autoconfig/cups-autoconfig
%{_libdir}/hal/hal-cups-autoconfig
%config %{_sysconfdir}/cups-autoconfig.conf
%{_datadir}/hal/fdi/policy/20thirdparty/10-cups-autoconfig.fdi
%{_datadir}/locale/en_US/LC_MESSAGES/cups-autoconfig.mo

%changelog -n cups-autoconfig 
* Mon Jul 09 2007 - crivera@novell.com
- Initial package 
