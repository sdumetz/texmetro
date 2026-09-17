#include "gltf_import.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "meshoptimizer.h"

#include <draco/compression/decode.h>

#include <vcg/complex/allocate.h>
#include <wrap/io_trimesh/io_mask.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

using namespace vcg;

namespace {

// ---------------------------------------------------------------------------
// EXT_meshopt_compression: cgltf only describes these buffer views, it never
// decodes them. We decode every compressed view up front and stash the
// result in buffer_view->data, which cgltf_buffer_view_data() (and thus
// every accessor reader) prefers over buffer->data when present.
// ---------------------------------------------------------------------------
bool DecodeMeshoptBufferViews(cgltf_data *data)
{
    for (cgltf_size i = 0; i < data->buffer_views_count; ++i) {
        cgltf_buffer_view &view = data->buffer_views[i];
        if (!view.has_meshopt_compression)
            continue;

        cgltf_meshopt_compression &mc = view.meshopt_compression;
        if (!mc.buffer || !mc.buffer->data) {
            std::cerr << "meshopt: buffer view refers to unavailable buffer data" << std::endl;
            return false;
        }
        if (mc.stride == 0) {
            std::cerr << "meshopt: invalid (zero) stride" << std::endl;
            return false;
        }

        const uint8_t *source = (const uint8_t *) mc.buffer->data + mc.offset;
        const size_t decodedSize = mc.count * mc.stride;
        void *destination = malloc(decodedSize ? decodedSize : 1);
        if (!destination) {
            std::cerr << "meshopt: failed to allocate " << decodedSize << " bytes" << std::endl;
            return false;
        }

        int error = -1;
        switch (mc.mode) {
        case cgltf_meshopt_compression_mode_attributes:
            error = meshopt_decodeVertexBuffer(destination, mc.count, mc.stride, source, mc.size);
            break;
        case cgltf_meshopt_compression_mode_triangles:
            error = meshopt_decodeIndexBuffer(destination, mc.count, mc.stride, source, mc.size);
            break;
        case cgltf_meshopt_compression_mode_indices:
            error = meshopt_decodeIndexSequence(destination, mc.count, mc.stride, source, mc.size);
            break;
        default:
            break;
        }
        if (error != 0) {
            std::cerr << "meshopt: decoding failed with error " << error << std::endl;
            free(destination);
            return false;
        }

        switch (mc.filter) {
        case cgltf_meshopt_compression_filter_octahedral:
            meshopt_decodeFilterOct(destination, mc.count, mc.stride);
            break;
        case cgltf_meshopt_compression_filter_quaternion:
            meshopt_decodeFilterQuat(destination, mc.count, mc.stride);
            break;
        case cgltf_meshopt_compression_filter_exponential:
            meshopt_decodeFilterExp(destination, mc.count, mc.stride);
            break;
        default:
            break;
        }

        // Freed by cgltf_free() (it frees buffer_view->data unconditionally).
        view.data = destination;
    }
    return true;
}

// ---------------------------------------------------------------------------
// KHR_draco_mesh_compression: decode each referenced buffer view into a
// draco::Mesh (cached, since several primitives could in principle share one).
// ---------------------------------------------------------------------------
using DracoCache = std::map<const cgltf_buffer_view *, std::unique_ptr<draco::Mesh>>;

const draco::Mesh *DecodeDracoBufferView(const cgltf_buffer_view *view, DracoCache &cache)
{
    auto it = cache.find(view);
    if (it != cache.end())
        return it->second.get();

    std::unique_ptr<draco::Mesh> decoded;
    if (view && view->buffer && view->buffer->data) {
        const char *bytes = (const char *) view->buffer->data + view->offset;
        draco::DecoderBuffer buffer;
        buffer.Init(bytes, view->size);

        draco::Decoder decoder;
        auto geotype = decoder.GetEncodedGeometryType(&buffer);
        if (geotype.ok() && geotype.value() == draco::TRIANGULAR_MESH) {
            auto meshStatus = decoder.DecodeMeshFromBuffer(&buffer);
            if (meshStatus.ok())
                decoded = std::move(meshStatus).value();
            else
                std::cerr << "draco: " << meshStatus.status().error_msg_string() << std::endl;
        } else {
            std::cerr << "draco: buffer view does not contain a triangular mesh" << std::endl;
        }
    }

    const draco::Mesh *result = decoded.get();
    cache[view] = std::move(decoded);
    return result;
}

// ---------------------------------------------------------------------------
// Per-primitive geometry, already resolved to plain double-precision arrays
// regardless of whether the source was plain, meshopt- or draco-compressed.
// ---------------------------------------------------------------------------
struct PrimitiveGeometry {
    std::vector<Point3d> pos;
    std::vector<Point3d> nrm;
    std::vector<Point2d> uv;
    std::vector<uint32_t> tri; // flattened triangle list, size % 3 == 0
    bool hasNormal = false;
    bool hasUV = false;
};

bool ExtractPrimitive(const cgltf_data *data, const cgltf_primitive &prim, DracoCache &dracoCache, PrimitiveGeometry &out)
{
    if (prim.type != cgltf_primitive_type_triangles) {
        std::cerr << "gltf: skipping a non-triangle primitive" << std::endl;
        return true; // not fatal: just contributes nothing
    }

    if (prim.has_draco_mesh_compression) {
        const draco::Mesh *dm = DecodeDracoBufferView(prim.draco_mesh_compression.buffer_view, dracoCache);
        if (!dm) {
            std::cerr << "gltf: failed to decode a KHR_draco_mesh_compression primitive" << std::endl;
            return false;
        }

        const uint32_t n = (uint32_t) dm->num_points();
        out.pos.resize(n);

        for (cgltf_size i = 0; i < prim.draco_mesh_compression.attributes_count; ++i) {
            const cgltf_attribute &a = prim.draco_mesh_compression.attributes[i];
            // In cgltf, a draco attribute's "data" pointer is really the draco
            // unique id disguised as an (unresolved) accessor pointer.
            const uint32_t uniqueId = (uint32_t) (a.data - data->accessors);
            const draco::PointAttribute *attr = dm->GetAttributeByUniqueId(uniqueId);
            if (!attr)
                continue;

            if (a.type == cgltf_attribute_type_position) {
                for (draco::PointIndex p(0); p < n; ++p) {
                    double v[3];
                    attr->ConvertValue<double>(attr->mapped_index(p), 3, v);
                    out.pos[p.value()] = Point3d(v[0], v[1], v[2]);
                }
            } else if (a.type == cgltf_attribute_type_normal) {
                out.nrm.resize(n);
                out.hasNormal = true;
                for (draco::PointIndex p(0); p < n; ++p) {
                    double v[3];
                    attr->ConvertValue<double>(attr->mapped_index(p), 3, v);
                    out.nrm[p.value()] = Point3d(v[0], v[1], v[2]);
                }
            } else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) {
                out.uv.resize(n);
                out.hasUV = true;
                for (draco::PointIndex p(0); p < n; ++p) {
                    double v[2];
                    attr->ConvertValue<double>(attr->mapped_index(p), 2, v);
                    out.uv[p.value()] = Point2d(v[0], 1.0 - v[1]); // glTF UV -> OBJ-style UV
                }
            }
        }

        const uint32_t faceCount = (uint32_t) dm->num_faces();
        out.tri.resize(faceCount * 3);
        for (draco::FaceIndex f(0); f < faceCount; ++f) {
            const draco::Mesh::Face &face = dm->face(f);
            out.tri[3 * f.value() + 0] = face[0].value();
            out.tri[3 * f.value() + 1] = face[1].value();
            out.tri[3 * f.value() + 2] = face[2].value();
        }
        return true;
    }

    const cgltf_accessor *posAcc = cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
    if (!posAcc) {
        std::cerr << "gltf: primitive has no POSITION attribute" << std::endl;
        return false;
    }

    const uint32_t n = (uint32_t) posAcc->count;
    out.pos.resize(n);
    {
        std::vector<float> tmp(n * 3);
        cgltf_accessor_unpack_floats(posAcc, tmp.data(), n * 3);
        for (uint32_t i = 0; i < n; ++i)
            out.pos[i] = Point3d(tmp[3 * i], tmp[3 * i + 1], tmp[3 * i + 2]);
    }

    if (const cgltf_accessor *nrmAcc = cgltf_find_accessor(&prim, cgltf_attribute_type_normal, 0)) {
        out.nrm.resize(n);
        out.hasNormal = true;
        std::vector<float> tmp(n * 3);
        cgltf_accessor_unpack_floats(nrmAcc, tmp.data(), n * 3);
        for (uint32_t i = 0; i < n; ++i)
            out.nrm[i] = Point3d(tmp[3 * i], tmp[3 * i + 1], tmp[3 * i + 2]);
    }

    if (const cgltf_accessor *uvAcc = cgltf_find_accessor(&prim, cgltf_attribute_type_texcoord, 0)) {
        out.uv.resize(n);
        out.hasUV = true;
        std::vector<float> tmp(n * 2);
        cgltf_accessor_unpack_floats(uvAcc, tmp.data(), n * 2);
        for (uint32_t i = 0; i < n; ++i)
            out.uv[i] = Point2d(tmp[2 * i], 1.0 - tmp[2 * i + 1]); // glTF UV -> OBJ-style UV
    }

    if (prim.indices) {
        const uint32_t ic = (uint32_t) prim.indices->count;
        out.tri.resize(ic);
        for (uint32_t i = 0; i < ic; ++i)
            out.tri[i] = (uint32_t) cgltf_accessor_read_index(prim.indices, i);
    } else {
        out.tri.resize(n);
        for (uint32_t i = 0; i < n; ++i)
            out.tri[i] = i;
    }

    return true;
}

Point3d TransformPoint(const float M[16], const Point3d &p)
{
    return Point3d(
        (double) M[0] * p.X() + (double) M[4] * p.Y() + (double) M[8]  * p.Z() + (double) M[12],
        (double) M[1] * p.X() + (double) M[5] * p.Y() + (double) M[9]  * p.Z() + (double) M[13],
        (double) M[2] * p.X() + (double) M[6] * p.Y() + (double) M[10] * p.Z() + (double) M[14]);
}

Point3d TransformNormal(const float M[16], const Point3d &n)
{
    const Point3d c0((double) M[0], (double) M[1], (double) M[2]);
    const Point3d c1((double) M[4], (double) M[5], (double) M[6]);
    const Point3d c2((double) M[8], (double) M[9], (double) M[10]);

    const double det = c0 * (c1 ^ c2);
    if (std::abs(det) < 1e-12)
        return n;

    Point3d result = (n.X() * (c1 ^ c2) + n.Y() * (c2 ^ c0) + n.Z() * (c0 ^ c1)) / det;
    result.Normalize();
    return result;
}

const cgltf_image *BaseColorImage(const cgltf_material *mat)
{
    if (!mat)
        return nullptr;
    if (mat->has_pbr_metallic_roughness && mat->pbr_metallic_roughness.base_color_texture.texture)
        return mat->pbr_metallic_roughness.base_color_texture.texture->image;
    if (mat->has_pbr_specular_glossiness && mat->pbr_specular_glossiness.diffuse_texture.texture)
        return mat->pbr_specular_glossiness.diffuse_texture.texture->image;
    return nullptr;
}

const char *ImageExtension(const cgltf_image *img)
{
    if (img->mime_type) {
        if (strcmp(img->mime_type, "image/png") == 0)
            return ".png";
        if (strcmp(img->mime_type, "image/jpeg") == 0)
            return ".jpg";
    }
    if (img->uri) {
        if (strstr(img->uri, ".png"))
            return ".png";
        if (strstr(img->uri, ".jpg") || strstr(img->uri, ".jpeg"))
            return ".jpg";
    }
    return ".img";
}

// Resolves a glTF image to a real file on disk, extracting it to a temporary
// file if it is embedded (GLB bufferView or data: URI). Downstream code
// (mesh.cpp, color_consistency.cpp) only ever deals with textures as files.
QString ExtractImage(const cgltf_image *img, const QDir &gltfDir, QTemporaryDir &tmpDir, int index)
{
    const char *ext = ImageExtension(img);

    if (img->buffer_view) {
        const cgltf_buffer_view *view = img->buffer_view;
        if (!view->buffer || !view->buffer->data)
            return QString();
        QString path = tmpDir.filePath(QString("texture%1%2").arg(index).arg(ext));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return QString();
        f.write((const char *) view->buffer->data + view->offset, (qint64) view->size);
        f.close();
        return path;
    }

    if (img->uri) {
        QString uri = QString::fromUtf8(img->uri);
        if (uri.startsWith(QLatin1String("data:"))) {
            int comma = uri.indexOf(QLatin1Char(','));
            if (comma < 0 || !uri.left(comma).contains(QLatin1String("base64")))
                return QString();

            QByteArray b64 = uri.mid(comma + 1).toLatin1();
            qint64 len = b64.size();
            qint64 pad = 0;
            if (len >= 1 && b64.endsWith('='))
                pad++;
            if (len >= 2 && b64.endsWith("=="))
                pad++;
            qint64 decodedSize = (len / 4) * 3 - pad;
            if (decodedSize <= 0)
                return QString();

            cgltf_options options = {};
            void *data = nullptr;
            if (cgltf_load_buffer_base64(&options, (cgltf_size) decodedSize, b64.constData(), &data) != cgltf_result_success)
                return QString();

            QString path = tmpDir.filePath(QString("texture%1%2").arg(index).arg(ext));
            QFile f(path);
            bool ok = f.open(QIODevice::WriteOnly);
            if (ok) {
                f.write((const char *) data, decodedSize);
                f.close();
            }
            free(data);
            return ok ? path : QString();
        }

        std::vector<char> buf(img->uri, img->uri + strlen(img->uri) + 1);
        cgltf_decode_uri(buf.data());
        return QFileInfo(gltfDir, QString::fromUtf8(buf.data())).absoluteFilePath();
    }

    return QString();
}

} // namespace

bool ImportGLTF(Mesh &m, const char *filename, int &loadmask)
{
    cgltf_options options = {};
    cgltf_data *data = nullptr;

    cgltf_result result = cgltf_parse_file(&options, filename, &data);
    if (result != cgltf_result_success) {
        std::cerr << "Failed to parse " << filename << " (cgltf error " << (int) result << ")" << std::endl;
        return false;
    }

    result = cgltf_load_buffers(&options, data, filename);
    if (result != cgltf_result_success) {
        std::cerr << "Failed to load buffers for " << filename << " (cgltf error " << (int) result << ")" << std::endl;
        cgltf_free(data);
        return false;
    }

    if (!DecodeMeshoptBufferViews(data)) {
        cgltf_free(data);
        return false;
    }

    struct Instance {
        const cgltf_node *node;
        const cgltf_primitive *prim;
    };
    std::vector<Instance> instances;
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node &node = data->nodes[i];
        if (!node.mesh)
            continue;
        for (cgltf_size j = 0; j < node.mesh->primitives_count; ++j)
            instances.push_back({&node, &node.mesh->primitives[j]});
    }
    if (instances.empty()) {
        // No scene graph at all: fall back to importing every primitive as-is.
        for (cgltf_size i = 0; i < data->meshes_count; ++i)
            for (cgltf_size j = 0; j < data->meshes[i].primitives_count; ++j)
                instances.push_back({nullptr, &data->meshes[i].primitives[j]});
    }

    DracoCache dracoCache;
    std::vector<PrimitiveGeometry> geoms(instances.size());
    size_t totalVerts = 0, totalTris = 0;
    bool anyUV = false, anyNormal = false;
    for (size_t i = 0; i < instances.size(); ++i) {
        if (!ExtractPrimitive(data, *instances[i].prim, dracoCache, geoms[i])) {
            cgltf_free(data);
            return false;
        }
        totalVerts += geoms[i].pos.size();
        totalTris += geoms[i].tri.size() / 3;
        anyUV = anyUV || geoms[i].hasUV;
        anyNormal = anyNormal || geoms[i].hasNormal;
    }

    m.Clear();
    m.name = QFileInfo(filename).fileName().toStdString();
    m.vert.reserve(totalVerts);
    m.face.reserve(totalTris);

    static QTemporaryDir textureTempDir;
    QDir gltfDir = QFileInfo(filename).absoluteDir();
    std::map<const cgltf_image *, int> imageIndex;

    for (size_t i = 0; i < instances.size(); ++i) {
        const cgltf_primitive &prim = *instances[i].prim;
        const PrimitiveGeometry &geo = geoms[i];
        if (geo.pos.empty())
            continue;

        float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (instances[i].node)
            cgltf_node_transform_world(instances[i].node, world);

        int texIndex = -1;
        if (const cgltf_image *img = BaseColorImage(prim.material)) {
            auto it = imageIndex.find(img);
            if (it != imageIndex.end()) {
                texIndex = it->second;
            } else {
                QString path = ExtractImage(img, gltfDir, textureTempDir, (int) imageIndex.size());
                if (path.isEmpty()) {
                    std::cerr << "Warning: could not resolve a texture image, some faces will be untextured" << std::endl;
                } else {
                    texIndex = (int) m.textures.size();
                    m.textures.push_back(path.toStdString());
                }
                imageIndex[img] = texIndex;
            }
        }

        const size_t vbase = m.vert.size();
        auto vi = tri::Allocator<Mesh>::AddVertices(m, geo.pos.size());
        for (size_t k = 0; k < geo.pos.size(); ++k, ++vi) {
            vi->P() = TransformPoint(world, geo.pos[k]);
            if (geo.hasNormal)
                vi->N() = TransformNormal(world, geo.nrm[k]);
        }

        const size_t triCount = geo.tri.size() / 3;
        auto fi = tri::Allocator<Mesh>::AddFaces(m, triCount);
        for (size_t t = 0; t < triCount; ++t, ++fi) {
            for (int c = 0; c < 3; ++c) {
                const uint32_t vidx = geo.tri[3 * t + c];
                fi->V(c) = &m.vert[vbase + vidx];
                if (geo.hasUV) {
                    fi->WT(c).U() = geo.uv[vidx].X();
                    fi->WT(c).V() = geo.uv[vidx].Y();
                    fi->WT(c).N() = texIndex >= 0 ? texIndex : 0;
                }
            }
        }
    }

    cgltf_free(data);

    loadmask = tri::io::Mask::IOM_VERTCOORD | tri::io::Mask::IOM_FACEINDEX;
    if (anyNormal)
        loadmask |= tri::io::Mask::IOM_VERTNORMAL;
    if (anyUV) {
        loadmask |= tri::io::Mask::IOM_WEDGTEXCOORD;
        if (imageIndex.size() > 1)
            loadmask |= tri::io::Mask::IOM_WEDGTEXMULTI;
    }

    return true;
}
