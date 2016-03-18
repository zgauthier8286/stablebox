Stablebox is a derived version of Busybox, intended for those who are more concerned with stability and a more defined-scope than is currently provided by Busybox.

Stablebox started life as a fork of Busybox at the 1.2.2.1 branch, where functionality was relatively mature and complete.  It's primary audience is embedded device developers who need a mature and stable product, and are primarily wanting additional code added only for bug fixes and important enhancements.

The project has the following primary goals:
  1. Maintain a stable code base -- modifications should be done on a limited and carefully measured scope, primarily focused on bug fixes.
  1. Reduce the feature set to only include the truly core Linux system utilities.  Larger, less integrated features should be removed, and instead should be supported by other projects than Stablebox.
  1. Not churning source code for style or naming-convention changes.  Becoming a functioning desktop replacement, or working on non-Linux OS will be avoided as well.

The target audience for this project is developers of embedded products running the Linux OS.

License:
As noted, the source code for this project is under the GPLv2 license.  Much discussion and legal headache resulted in this being the license for Busybox after the 1.2.2.1 release, the point at which Stablebox was forked.