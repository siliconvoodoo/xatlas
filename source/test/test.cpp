/*
MIT License

Copyright (c) 2018-2020 Jonathan Young

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201)
#endif
#include <tiny_obj_loader.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <xatlas.h>

#define MODEL_PATH "../../models/"
#define ASSERT(_condition) if (!(_condition)) { logf("[FAILED] '%s' %s %d\n", #_condition, __FILE__, __LINE__); assert(_condition); }

static FILE *logFile = nullptr;
static int s_failureCount = 0; // Counts every logged [FAILED], whether from ASSERT or a load error.

int logf(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	char buffer[2048];
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	if (strstr(buffer, "[FAILED]"))
		s_failureCount++;
	if (logFile) {
		fprintf(logFile, "%s", buffer);
		fflush(logFile);
	}
	return printf("%s", buffer);
}

struct AtlasResult {
	uint32_t chartCount;
};

bool generateAtlas(const char *filename, bool useUvMesh, AtlasResult *result)
{
	logf("%s\n", filename);
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string err;
	if (!tinyobj::LoadObj(shapes, materials, err, filename, NULL, tinyobj::triangulation)) {
		logf("   [FAILED]: %s\n", err.c_str());
		return false;
	}
	if (shapes.size() == 0) {
		logf("   [FAILED]: no shapes in obj file\n");
		return false;
	}
	const clock_t start = clock();
	xatlas::Atlas *atlas = xatlas::Create(true);
	int missingUvsCount = 0;
	for (int i = 0; i < (int)shapes.size(); i++) {
		const tinyobj::mesh_t &objMesh = shapes[i].mesh;
		if (useUvMesh) {
			xatlas::UvMeshDecl decl;
			if (!objMesh.texcoords.empty()) {
				decl.vertexUvData = objMesh.texcoords.data();
				decl.vertexCount = (int)objMesh.texcoords.size() / 2;
				decl.vertexStride = sizeof(float) * 2;
				decl.indexCount = (int)objMesh.indices.size();
				decl.indexData = objMesh.indices.data();
				decl.indexFormat = xatlas::IndexFormat::UInt32;
				decl.faceMaterialData = (const uint32_t *)objMesh.material_ids.data();
			} else {
				missingUvsCount++;
			}
			xatlas::AddMeshError error = xatlas::AddUvMesh(atlas, decl);
			if (error != xatlas::AddMeshError::Success) {
				xatlas::Destroy(atlas);
				logf("   [FAILED]: Error adding UV mesh %d '%s': %s\n", i, shapes[i].name.c_str(), xatlas::StringForEnum(error));
				return false;
			}
		} else {
			xatlas::MeshDecl decl;
			decl.vertexCount = (int)objMesh.positions.size() / 3;
			decl.vertexPositionData = objMesh.positions.data();
			decl.vertexPositionStride = sizeof(float) * 3;
			if (!objMesh.normals.empty()) {
				decl.vertexNormalData = objMesh.normals.data();
				decl.vertexNormalStride = sizeof(float) * 3;
			}
			if (!objMesh.texcoords.empty()) {
				decl.vertexUvData = objMesh.texcoords.data();
				decl.vertexUvStride = sizeof(float) * 2;
			}
			decl.indexCount = (int)objMesh.indices.size();
			decl.indexData = objMesh.indices.data();
			decl.indexFormat = xatlas::IndexFormat::UInt32;
			xatlas::AddMeshError error = xatlas::AddMesh(atlas, decl);
			if (error != xatlas::AddMeshError::Success) {
				xatlas::Destroy(atlas);
				logf("   [FAILED]: Error adding mesh %d '%s': %s\n", i, shapes[i].name.c_str(), xatlas::StringForEnum(error));
				return false;
			}
		}
	}
	if (missingUvsCount > 0)
		logf("   %u/%u meshes missing UVs\n", missingUvsCount, (int)shapes.size());
	xatlas::Generate(atlas);
	const clock_t end = clock();
	logf("   %g ms\n", (end - start) * 1000.0 / (double)CLOCKS_PER_SEC);
	for (uint32_t i = 0; i < atlas->meshCount; i++) {
		const xatlas::Mesh &mesh = atlas->meshes[i];
		const tinyobj::mesh_t &objMesh = shapes[i].mesh;
		if (useUvMesh) {
			if (objMesh.texcoords.empty()) {
				ASSERT(mesh.indexCount == 0);
			} else {
				// Index count shouldn't change.
				ASSERT(mesh.indexCount == objMesh.indices.size());
			}
			// Vertex count shouldn't change.
			ASSERT(mesh.vertexCount == objMesh.texcoords.size() / 2);
		} else {
			// Index count shouldn't change.
			ASSERT(mesh.indexCount == objMesh.indices.size());
			// Vertex count should be equal or greater.
			ASSERT(mesh.vertexCount >= objMesh.positions.size() / 3);
			// Index order should be preserved.
			for (uint32_t j = 0; j < mesh.indexCount; j++) {
				const xatlas::Vertex &vertex = mesh.vertexArray[mesh.indexArray[j]];
				ASSERT(vertex.xref == objMesh.indices[j]);
			}
		}
	}
	if (result)
		result->chartCount = atlas->chartCount;
	xatlas::Destroy(atlas);
	return true;
}

// Feed a mesh as raw, non-indexed triangles: MeshDecl::indexData null, indexCount 0.
// Regression test: this path used to write into an empty (null buffer) array in AddMesh and crash.
bool generateAtlasNonIndexed(const char *filename, AtlasResult *result)
{
	logf("%s (non-indexed)\n", filename);
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string err;
	if (!tinyobj::LoadObj(shapes, materials, err, filename, NULL, tinyobj::triangulation)) {
		logf("   [FAILED]: %s\n", err.c_str());
		return false;
	}
	if (shapes.size() == 0) {
		logf("   [FAILED]: no shapes in obj file\n");
		return false;
	}
	xatlas::Atlas *atlas = xatlas::Create(true);
	std::vector<std::vector<float>> expandedPositions(shapes.size());
	for (int i = 0; i < (int)shapes.size(); i++) {
		const tinyobj::mesh_t &objMesh = shapes[i].mesh;
		// Expand indexed triangles into a flat vertex stream.
		std::vector<float> &positions = expandedPositions[i];
		positions.reserve(objMesh.indices.size() * 3);
		for (size_t j = 0; j < objMesh.indices.size(); j++) {
			const unsigned int v = objMesh.indices[j];
			positions.push_back(objMesh.positions[v * 3 + 0]);
			positions.push_back(objMesh.positions[v * 3 + 1]);
			positions.push_back(objMesh.positions[v * 3 + 2]);
		}
		xatlas::MeshDecl decl;
		decl.vertexCount = (uint32_t)positions.size() / 3;
		decl.vertexPositionData = positions.data();
		decl.vertexPositionStride = sizeof(float) * 3;
		// No indexData / indexCount: non-indexed path.
		xatlas::AddMeshError error = xatlas::AddMesh(atlas, decl);
		if (error != xatlas::AddMeshError::Success) {
			xatlas::Destroy(atlas);
			logf("   [FAILED]: Error adding mesh %d '%s': %s\n", i, shapes[i].name.c_str(), xatlas::StringForEnum(error));
			return false;
		}
	}
	xatlas::Generate(atlas);
	for (uint32_t i = 0; i < atlas->meshCount; i++) {
		const xatlas::Mesh &mesh = atlas->meshes[i];
		const uint32_t inputVertexCount = (uint32_t)expandedPositions[i].size() / 3;
		// One index per input vertex, identity order.
		ASSERT(mesh.indexCount == inputVertexCount);
		ASSERT(mesh.vertexCount >= inputVertexCount);
		for (uint32_t j = 0; j < mesh.indexCount; j++) {
			const xatlas::Vertex &vertex = mesh.vertexArray[mesh.indexArray[j]];
			ASSERT(vertex.xref == j);
		}
	}
	if (result)
		result->chartCount = atlas->chartCount;
	xatlas::Destroy(atlas);
	return true;
}

// Load an obj without triangulating: quads/ngons are passed to xatlas via MeshDecl::faceVertexCount,
// exercising the polygon code path (Triangulator, trianglesToPolygonIDs, polygon-mate chart logic).
bool generateAtlasPolygons(const char *filename, bool checkIndexOrder, AtlasResult *result)
{
	logf("%s (polygons)\n", filename);
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string err;
	if (!tinyobj::LoadObj(shapes, materials, err, filename, NULL, 0)) {
		logf("   [FAILED]: %s\n", err.c_str());
		return false;
	}
	if (shapes.size() == 0) {
		logf("   [FAILED]: no shapes in obj file\n");
		return false;
	}
	xatlas::Atlas *atlas = xatlas::Create(true);
	std::vector<std::vector<uint32_t>> faceVertexCounts(shapes.size());
	for (int i = 0; i < (int)shapes.size(); i++) {
		const tinyobj::mesh_t &objMesh = shapes[i].mesh;
		// tinyobj stores per-face vertex counts as uint8, xatlas wants uint32.
		std::vector<uint32_t> &faceVertexCount = faceVertexCounts[i];
		faceVertexCount.assign(objMesh.num_vertices.begin(), objMesh.num_vertices.end());
		xatlas::MeshDecl decl;
		decl.vertexCount = (uint32_t)objMesh.positions.size() / 3;
		decl.vertexPositionData = objMesh.positions.data();
		decl.vertexPositionStride = sizeof(float) * 3;
		decl.indexCount = (uint32_t)objMesh.indices.size();
		decl.indexData = objMesh.indices.data();
		decl.indexFormat = xatlas::IndexFormat::UInt32;
		decl.faceVertexCount = faceVertexCount.data();
		decl.faceCount = (uint32_t)faceVertexCount.size();
		xatlas::AddMeshError error = xatlas::AddMesh(atlas, decl);
		if (error != xatlas::AddMeshError::Success) {
			xatlas::Destroy(atlas);
			logf("   [FAILED]: Error adding mesh %d '%s': %s\n", i, shapes[i].name.c_str(), xatlas::StringForEnum(error));
			return false;
		}
	}
	xatlas::Generate(atlas);
	for (uint32_t i = 0; i < atlas->meshCount; i++) {
		const xatlas::Mesh &mesh = atlas->meshes[i];
		const tinyobj::mesh_t &objMesh = shapes[i].mesh;
		// Polygon index count shouldn't change.
		ASSERT(mesh.indexCount == objMesh.indices.size());
		ASSERT(mesh.vertexCount >= objMesh.positions.size() / 3);
		if (checkIndexOrder) {
			for (uint32_t j = 0; j < mesh.indexCount; j++) {
				const xatlas::Vertex &vertex = mesh.vertexArray[mesh.indexArray[j]];
				ASSERT(vertex.xref == objMesh.indices[j]);
			}
		}
	}
	if (result)
		result->chartCount = atlas->chartCount;
	xatlas::Destroy(atlas);
	return true;
}

// Add the same mesh many times to one atlas. The AddMesh task queue grows while worker threads
// are already draining it; regression test for the queue realloc use-after-free.
bool generateAtlasRepeated(const char *filename, uint32_t repeatCount, AtlasResult *result)
{
	logf("%s (x%u meshes)\n", filename, repeatCount);
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string err;
	if (!tinyobj::LoadObj(shapes, materials, err, filename, NULL, tinyobj::triangulation)) {
		logf("   [FAILED]: %s\n", err.c_str());
		return false;
	}
	if (shapes.size() == 0) {
		logf("   [FAILED]: no shapes in obj file\n");
		return false;
	}
	const tinyobj::mesh_t &objMesh = shapes[0].mesh;
	xatlas::MeshDecl decl;
	decl.vertexCount = (uint32_t)objMesh.positions.size() / 3;
	decl.vertexPositionData = objMesh.positions.data();
	decl.vertexPositionStride = sizeof(float) * 3;
	decl.indexCount = (uint32_t)objMesh.indices.size();
	decl.indexData = objMesh.indices.data();
	decl.indexFormat = xatlas::IndexFormat::UInt32;
	xatlas::Atlas *atlas = xatlas::Create(true);
	for (uint32_t n = 0; n < repeatCount; n++) {
		// Deliberately no meshCountHint: keeps the task group queue unreserved so it reallocs while workers run.
		xatlas::AddMeshError error = xatlas::AddMesh(atlas, decl);
		if (error != xatlas::AddMeshError::Success) {
			xatlas::Destroy(atlas);
			logf("   [FAILED]: Error adding mesh %u: %s\n", n, xatlas::StringForEnum(error));
			return false;
		}
	}
	xatlas::Generate(atlas);
	ASSERT(atlas->meshCount == repeatCount);
	if (result)
		result->chartCount = atlas->chartCount;
	xatlas::Destroy(atlas);
	return true;
}

// A polygon face referencing an out-of-range vertex must fail with IndexOutOfRange
// (and free the polygon mapping it allocated - regression test for a leak on that path).
void testPolygonIndexOutOfRange()
{
	logf("polygon index out of range\n");
	xatlas::Atlas *atlas = xatlas::Create(true);
	const float positions[] = {
		0.0f, 0.0f, 0.0f,
		1.0f, 0.0f, 0.0f,
		1.0f, 1.0f, 0.0f,
		0.0f, 1.0f, 0.0f
	};
	// A valid mesh first, so the atlas has a task group and Destroy after the failure below is exercised too.
	{
		const uint16_t indices[] = { 0, 1, 2 };
		xatlas::MeshDecl decl;
		decl.vertexCount = 4;
		decl.vertexPositionData = positions;
		decl.vertexPositionStride = sizeof(float) * 3;
		decl.indexCount = 3;
		decl.indexData = indices;
		decl.indexFormat = xatlas::IndexFormat::UInt16;
		ASSERT(xatlas::AddMesh(atlas, decl) == xatlas::AddMeshError::Success);
	}
	{
		const uint16_t indices[] = { 0, 1, 99, 3 }; // 99 is out of range
		const uint32_t faceVertexCount[] = { 4 };
		xatlas::MeshDecl decl;
		decl.vertexCount = 4;
		decl.vertexPositionData = positions;
		decl.vertexPositionStride = sizeof(float) * 3;
		decl.indexCount = 4;
		decl.indexData = indices;
		decl.indexFormat = xatlas::IndexFormat::UInt16;
		decl.faceVertexCount = faceVertexCount;
		decl.faceCount = 1;
		ASSERT(xatlas::AddMesh(atlas, decl) == xatlas::AddMeshError::IndexOutOfRange);
	}
	xatlas::Destroy(atlas);
}

#ifdef _MSC_VER
void processFilesRecursive(const char *path, bool useUvMesh)
{
	WIN32_FIND_DATAA ffd;
	const char lastChar = path[strlen(path) - 1];
	char cleanPath[256];
	sprintf_s(cleanPath, sizeof(cleanPath), "%s%s", path, lastChar == '/' || lastChar == '\\' ? "" : "/");
	char findPath[256];
	sprintf_s(findPath, sizeof(findPath), "%s*", cleanPath);
	HANDLE hFind = FindFirstFileA(findPath, &ffd);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			if (strcmp(ffd.cFileName, ".") == 0 || strcmp(ffd.cFileName, "..") == 0)
				continue;
			if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				char childPath[256];
				sprintf_s(childPath, sizeof(childPath), "%s%s/", cleanPath, ffd.cFileName);
				processFilesRecursive(childPath, useUvMesh);
			} else {
				const char *dot = strrchr(ffd.cFileName, (int)'.');
				if (!dot)
					continue;
				if (_strcmpi(dot + 1, "obj") != 0)
					continue;
				char filename[256];
				sprintf_s(filename, sizeof(filename), "%s%s", cleanPath, ffd.cFileName);
				if (!generateAtlas(filename, useUvMesh, nullptr))
					exit(1);
			}
		}
		while (FindNextFileA(hFind, &ffd) != 0);
		FindClose(hFind);
	}
}
#endif

int main(int argc, char **argv)
{
#ifdef _MSC_VER
	if (fopen_s(&logFile, "test.log", "w") != 0)
		logFile = nullptr;
#else
	logFile = fopen("test.log", "w");
#endif
	xatlas::SetPrint(logf, false);
	if (argc > 1) {
		const char *searchPath = argv[1];
		bool useUvMesh = false;
		if (argc > 2 && strncmp(argv[1], "--uv", 4) == 0) {
			useUvMesh = true;
			searchPath = argv[2];
		}
		logf("Search path is '%s'\n", searchPath);
#ifdef _MSC_VER
		processFilesRecursive(searchPath, useUvMesh);
#else
		(void)useUvMesh;
		logf("not implemented\n");
#endif
	} else {
		AtlasResult result;
		if (generateAtlas(MODEL_PATH "cube.obj", false, &result)) {
			ASSERT(result.chartCount == 6);
		}
		if (generateAtlas(MODEL_PATH "degenerate_edge.obj", false, &result)) {
			ASSERT(result.chartCount == 1);
		}
		// double sided quad
		if (generateAtlas(MODEL_PATH "double_sided.obj", false, &result)) {
			ASSERT(result.chartCount == 2);
		}
		if (generateAtlas(MODEL_PATH "duplicate_edge.obj", false, &result)) {
			ASSERT(result.chartCount == 2);
		}
		if (generateAtlas(MODEL_PATH "gazebo.obj", false, &result)) {
			// 333 with vanilla upstream xatlas; 332 since the local segmentation changes
			// (merge criteria, seam epsilon, degenerate-face attribution).
			ASSERT(result.chartCount == 332);
		}
		if (generateAtlas(MODEL_PATH "zero_area_face.obj", false, &result)) {
			ASSERT(result.chartCount == 0);
		}
		if (generateAtlas(MODEL_PATH "zero_length_edge.obj", false, &result)) {
			ASSERT(result.chartCount == 1);
		}
		// Non-indexed AddMesh (used to crash on an empty-array write).
		if (generateAtlasNonIndexed(MODEL_PATH "quad.obj", &result)) {
			ASSERT(result.chartCount == 1);
		}
		// Quads through MeshDecl::faceVertexCount (polygon code path).
		if (generateAtlasPolygons(MODEL_PATH "polygon_cube.obj", true, &result)) {
			ASSERT(result.chartCount == 6);
		}
		// NGon with a collinear corner: its triangulation contains an exactly-zero-area
		// triangle that enters the pipeline (used to be able to hang seed placement).
		if (generateAtlasPolygons(MODEL_PATH "degenerate_polygon.obj", false, &result)) {
			ASSERT(result.chartCount == 1);
		}
		// Grow the AddMesh task queue while workers drain it (used to be a realloc use-after-free).
		if (generateAtlasRepeated(MODEL_PATH "cube.obj", 64, &result)) {
			ASSERT(result.chartCount == 64 * 6);
		}
		// Out-of-range polygon index error path (used to leak the polygon mapping).
		testPolygonIndexOutOfRange();
	}
	if (s_failureCount == 0)
		logf("SUCCESS: all tests passed.\n");
	else
		logf("FAILURE: %d failure(s), see [FAILED] lines above.\n", s_failureCount);
	return s_failureCount == 0 ? 0 : 1;
}
