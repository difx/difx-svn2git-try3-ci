#!/usr/bin/env perl
if (!$ARGV[0]) {
  die "exiting: please give a tag name";
}

$tag = $ARGV[0];
system "svn mkdir $tag";

system "svn mkdir $tag/applications";
system "svn copy ../applications/DiFXGUI/trunk      ./$tag/applications/DiFXGUI";
system "svn copy ../applications/calc/trunk         ./$tag/applications/calc";
system "svn copy ../applications/calcserver/trunk   ./$tag/applications/calcserver";
system "svn copy ../applications/difx2fits/trunk    ./$tag/applications/difx2fits";
system "svn copy ../applications/difx2ms/trunk      ./$tag/applications/difx2ms";
system "svn copy ../applications/difx_monitor/trunk ./$tag/applications/difx_monitor";
system "svn copy ../applications/mk5daemon/trunk    ./$tag/applications/mk5daemon";

system "svn mkdir $tag/deprecated";
system "svn copy ../deprecated/fringetool/trunk     ./$tag/deprecated/fringetool";
system "svn copy ../deprecated/vlba_utils/trunk     ./$tag/deprecated/vlba_utils";

system "svn copy ../mpifxcorr/trunk                 ./$tag/mpifxcorr";

system "svn copy ../utilities/trunk                 ./$tag/utilities";

system "svn copy ../README.txt                      ./$tag/README.txt";

print "The standard files/directories for a full distribution of DiFX have just
been copied. Commit this change if you want to tag this as a release in the
repository";

