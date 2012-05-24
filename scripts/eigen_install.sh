#!/bin/bash


_Box () {
    str="$@"
    len=$((${#str}+4))
    for (( i=1; i<=$len; i++ )); do echo -n '-'; done;
    echo; echo "| "$str" |";
    for (( i=1; i<=$len; i++ )); do echo -n '-'; done;
    echo
}

function test_for_eigen {
   if [ ! -d ./deps/eigen-eigen-3.0.2/ ]; then
      eigenfound=1;
      echo "Eigen was found at: $1/deps/eigen-eigen-3.0.2/"
   fi
}


function download_file_with_forward {
  echo "Downloading $2 from $1 ..."
  wgetcmd=`which wget`
  if [ -z $wgetcmd ] ; then
    curlcmd=`which curl`
    if [ -z $curlcmd ] ; then
      echo "Unable to find either curl or wget! Cannot proceed with automatic install."
      exit 1
    fi
    curl -L $1 -o $2
  else
    wget  --no-check-certificate $1 -O $2
  fi
}


if  [ $eigenfound ]; then
    return
fi

echo " ============= Source installation of Eigen =============== "
  # detect wget
echo "The script will now proceed to download Eigen and try to set it up"
echo
echo "Press Enter to proceed,"
echo "or Ctrl-C to break and install Eigen"
if [ -z $always_yes ] ; then
  read
fi
 
echo "Downloading Eigen 3.0.2 from www.tuxfamily.com... into `pwd`"
download_file_with_forward https://bitbucket.org/eigen/eigen/get/3.0.2.tar.gz \
    ./deps/eigen-3.0.2.tar.gz
  # unpack
cd ./deps/
set -e
mkdir eigen-3.0.2
tar --strip-components=1 -xzvf eigen-3.0.2.tar.gz -C eigen-3.0.2
set +e
echo "Eigen setup success!"
cd ..