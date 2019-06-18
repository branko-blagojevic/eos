.. highlight:: rst

.. _space-policies:

Space Policies
==============

Space policies are set using the space configuration CLI.

The following policies can be configured

.. epigraph::

   ============= ==============================================
   key           values
   ============= ==============================================
   layout        plain,replica,raid5,raid6,raiddp,archive,qrain           
   nstripes      1..255           
   checksum      adler,md5,sha1,crc32,crc32c        
   blockchecksum adler,md5,sha1,crc32,crc32c           
   blocksize     4k,64k,128k,512k,1M,4M,16M,64M           
   ============= ==============================================


Setting space policies
----------------------


.. code-block:: bash

   # configure raid6 layout   
   eos space config default space.policy.layout=raid6

   # configure 10 stripes
   eos space config default space.nstripes=10 

   # configure adler file checksumming
   eos space config default space.checksum=adler

   # configure crc32c block checksumming
   eos space config default space.blockchecksum=crc32c

   # configure 1M blocksizes
   eos space config default space.blocksize=1M

Local Overwrites
----------------

The space polcies are overwritten by the local extended attribute settings of the parent directory

.. epigraph::

   ============= ===================================================
   key           local xattr
   ============= ===================================================
   layout        sys.forced.layout, user.forced.layout
   nstripes      sys.forced.nstripes, user.forced.nstripes
   checksum      sys.forced.checksum, user.forced.checksum
   blockchecksum sys.forced.blockchecksum, user.forced.blockchecksum   
   blocksize     sys.forced.blocksize, user.forced.blocksize
   ============= ===================================================


Clients can select the space ( and its default policies ) by adding ``eos.space=<space>`` to the CGI query of an URL.


Deleting space policies
-----------------------

Policies are deleted by setting a space policy with `value=remove` e.g.

.. code-block:: bash

   # delete a policy entry
   eos space config default space.ppolicy.layout=remove


Displaying space policies
-------------------------

Policies are displayd using the ``space status`` command:

.. code-block:: bash

   eos space status default

   # ------------------------------------------------------------------------------------
   # Space Variables
   # ....................................................................................
   autorepair                       := off
   ...
   policy.blockchecksum             := crc32c
   policy.blocksize                 := 1M
   policy.checksum                  := adler
   policy.layout                    := replica
   policy.nstripes                  := 2
   ...


 
 
