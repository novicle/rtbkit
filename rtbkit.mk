SUBDIRS := jml tinyxml2 googleurl leveldb soa rtbkit

PREMAKE := 1

$(eval $(call include_sub_makes,$(SUBDIRS)))


PREMAKE := 0

$(eval $(call include_sub_makes,$(SUBDIRS)))
