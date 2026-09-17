#ifndef GLTF_IMPORT_H
#define GLTF_IMPORT_H

#include "mesh.h"

// Loads a .gltf or .glb file into m, decoding EXT_meshopt_compression and
// KHR_draco_mesh_compression buffer views as needed. Sets loadmask the same
// way vcg::tri::io::Importer does (see wrap/io_trimesh/io_mask.h).
bool ImportGLTF(Mesh &m, const char *filename, int &loadmask);

#endif // GLTF_IMPORT_H
