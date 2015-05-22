A utility to auto-configure printers using CUPS.

# Introduction #

The purpose of this project is to improve the desktop printing experience.

# cups-autoconfig #

**Workflow**

This is a smally utility that runs, via a HAL callout, whenever a local printer is added or removed.  It gathers information via HAL, CUPS, and the CUPS backends to determine the appropriate action.

Settings for this utility are controlled via /etc/cups-autoconfig.conf.  It contains the following values:

ConfigureNewPrinters - Controls local printers being auto-configured on addition.

DisablePrintersOnRemoval - Controls local printers being disabled on removal.

Debug - Controls debugging output /var/log/cups-autoconfig.log.

DefaultCUPSPolicy - The name of the CUPS policy to assign to newly configured printers.

When a local printer is added to the system cups-autoconfig determines if the printer is already configured.  If the printer is configured then the printer is enabled.  If the printer is not configured then cups-autoconfig attempts to select the appropriate driver for the new printer and, if successful, creates a new print queue.  Newly configured printers will be assigned the DefaultCUPSPolicy.

When a local printer is removed the DisablePrintersOnRemoval variable is consulted.  If the variable is set to "yes" then the printer is disabled.

**HAL device properties**

If a printer has been successfully configured then cups-autoconfig will add "printer.configured" and "printer.display\_name" properties to the HAL printer device.  Policy applications, like gnome-volume-manager, can check for these properties to determine whether they should run their printer configuration tool or display a message saying that the printer has been configured.

# Removal of the HAL backend from cups-backends #

Ideally, HAL would be used for all hardware detection, but using the HAL backend complicates the process without buying us anything.  The usb backend is what is supported upstream and has to be supported in openSUSE.  Both backends detect the same printers and require complicated uri mapping schemes to avoid ambiguities.  This is more work that it's worth.  For 10.3 the hal backend will be silenced.  It won't report any detected printers, but existing hal backend printers will still work.

# CUPS Changes #

One of the major requirements for this project was to allow local users to perform reasonable actions (add, modify, remove, etc) on local printers.  Policies were added to CUPS 1.2, which is what we decided to use for this.  A few small changes to CUPS were required in order to smoothly integrate policy support.

**A relaxed policy**

A new CUPS policy has been defined to allow normal users to perform operations on local printers.  The policy allows users to add new printers, remove existing printers, pause printers, resume printers, and modify printer attributes.  The is the default policy used by cups-autoconfig for new local printers.

**Domain socket authentication**

The CUPS policy layer handles authorization, but not authentication.  Support was added to CUPS to retrieve the SO\_PEERCRED from the local CUPS domain socket in order to verify the identity of the user performing an IPP operation.  This allows users to perform an operation on local printers without typing their password again.  This patch has been accepted upstream for CUPS 1.3.  It works well for CUPS 1.2 too.

**Testing Policy Information**

Although CUPS has support for policies it doesn't have a reasonable way to figure out if a  user can perform an action given a policy.  It doesn't have a way to get the list of current policies.  It doesn't have an API for creating, editing, or removing policies.  Nice.  The inflexibility of the CUPS policies makes them almost unusable on the desktop.  PolicyKit should be used when it is ready.

# libgnomecups Changes #

There was a small bug fix to set the CUPS password callback for all threads in the thread pool, not just the main thread.  Without this fix threads will use the default callback, which just blocks for input on standard in.  Not good.

# gnome-cups-manager Changes #

**Removing the silly admin widgets**

All "Become Administrator" widgets have been removed.  These looked silly and aren't needed anymore.

There was a small bug fix to correctly find the user's lpoptions file.

**Fixing the authentication callback**

A small change was made to gnome-cups-add to make the add\_printer operation asynchronous.  Depending on the policy configuration, a user may be prompted for credentials to perform a given operation.  gnome-cups-add was attempting to add the printer synchronously, which was blocking the main gtk thread, which blocked the display of the authentication dialog.  This is fixed now.

# gnome-volume-manager Changes #

A small patch was added to gnome-volume-manager to make it aware of the previously mentioned HAL device properties.  When gvm receives a new printer event it will check for the "printer.configured" and "printer.display\_name" properties.  If it finds these properties then it will use libnotify to tell the user that their printer has been configured.  If these properties are missing then it will run it's default printer configuration tool for manual intervention.

# TODO #

Lots and lots of testing.