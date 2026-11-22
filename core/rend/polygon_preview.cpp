#include "polygon_preview.h"
#include "transform_matrix.h"
#include <glm/gtc/type_ptr.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace polygonPreview
{
namespace
{
std::mutex mutex;
std::atomic<u64> activeGeneration{0};
u64 nextGeneration = 0;
std::shared_ptr<const Frame> latest;
std::chrono::steady_clock::time_point nextCapture;

bool finite(glm::vec3 p)
{
	return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
}

void setEnabled(bool enabled)
{
	if (enabled == (activeGeneration.load(std::memory_order_relaxed) != 0))
		return;
	std::lock_guard<std::mutex> lock(mutex);
	activeGeneration = enabled ? ++nextGeneration : 0;
	latest.reset();
	nextCapture = {};
}

void clear()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (activeGeneration != 0)
		activeGeneration = ++nextGeneration;
	latest.reset();
	nextCapture = {};
}

std::shared_ptr<const Frame> snapshot()
{
	std::lock_guard<std::mutex> lock(mutex);
	return latest;
}

Capture::Capture(const rend_context& context) : context(context)
{
	generation = activeGeneration.load(std::memory_order_relaxed);
	if (generation == 0 || context.isRTT || context.globClip.x <= 0 || context.globClip.y <= 0)
		return;
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto now = std::chrono::steady_clock::now();
		if (generation != activeGeneration || now < nextCapture)
			return;
		// Bound collection cost independently of the game's frame rate.
		nextCapture = now + std::chrono::milliseconds(33);
	}
	frame = std::make_shared<Frame>();
	frame->aspectRatio = getOutputFramebufferAspectRatio();
	rotate = config::Rotate90;
	screenMatrix = TransformMatrix(context, std::max(1u, context.framebufferWidth),
			std::max(1u, context.framebufferHeight)).GetNormalMatrix();
}

void Capture::addEdge(glm::vec3 a, glm::vec3 b)
{
	if (!finite(a) || !finite(b))
		return;
	// Homogeneous clipping also handles Naomi 2 edges crossing the camera plane.
	// Clip before dividing or converting to GDI integers, even for huge vertices.
	const glm::vec3 planes[] = {{0, 0, 1}, {1, 0, 0}, {-1, 0, 1}, {0, 1, 0}, {0, -1, 1}};
	for (int i = 0; i < 5; i++)
	{
		double da = glm::dot(glm::dvec3(planes[i]), glm::dvec3(a)) - (i == 0 ? 1e-6 : 0);
		double db = glm::dot(glm::dvec3(planes[i]), glm::dvec3(b)) - (i == 0 ? 1e-6 : 0);
		if (da < 0 && db < 0)
			return;
		if ((da < 0) != (db < 0))
		{
			glm::vec3 intersection = glm::dvec3(a) + (glm::dvec3(b) - glm::dvec3(a)) * (da / (da - db));
			if (da < 0)
				a = intersection;
			else
				b = intersection;
		}
	}
	if (a.z <= 0 || b.z <= 0)
		return;
	a /= a.z;
	b /= b.z;
	if (!finite(a) || !finite(b))
		return;
	glm::vec2 start = glm::clamp(glm::vec2(a), glm::vec2(0), glm::vec2(1));
	glm::vec2 end = glm::clamp(glm::vec2(b), glm::vec2(0), glm::vec2(1));
	if (rotate)
	{
		start = {start.y, 1.f - start.x};
		end = {end.y, 1.f - end.x};
	}
	if (start != end)
		frame->edges.push_back({start, end});
}

void Capture::addPass(const RenderPass& pass, const RenderPass& previousPass)
{
	if (!frame)
		return;
	auto strips = [&](const std::vector<PolyParam>& polys, u32 first, u32 last) {
		for (size_t p = first; p < std::min<size_t>(last, polys.size()); p++)
		{
			const PolyParam& poly = polys[p];
			if (poly.count < 3 || poly.first > context.verts.size()
					|| poly.count > context.verts.size() - poly.first)
				continue;
			glm::mat4 modelProjection(1.f);
			if (poly.isNaomi2())
			{
				if (poly.mvMatrix < 0 || (size_t)poly.mvMatrix >= context.matrices.size()
						|| (size_t)poly.projMatrix >= context.matrices.size())
					continue;
				modelProjection = glm::make_mat4(context.matrices[poly.projMatrix].mat)
						* glm::make_mat4(context.matrices[poly.mvMatrix].mat);
			}
			auto project = [&](const Vertex& v) -> glm::vec3 {
				glm::vec4 position = poly.isNaomi2()
						? modelProjection * glm::vec4(v.x, v.y, v.z, 1.f)
						: glm::vec4(v.x, v.y, 0.f, 1.f);
				position = screenMatrix * position;
				return {(position.x + position.w) * 0.5f, (position.y + position.w) * 0.5f, position.w};
			};
			glm::vec3 a = project(context.verts[poly.first]);
			glm::vec3 b = project(context.verts[poly.first + 1]);
			for (u32 v = 2; v < poly.count; v++)
			{
				if (frame->triangles >= MaxTriangles)
				{
					frame->limited = true;
					return;
				}
				glm::vec3 c = project(context.verts[poly.first + v]);
				frame->triangles++;
				// Repeated strip vertices are connectors, not visible triangles.
				if (finite(a) && finite(b) && finite(c) && a != b && b != c && c != a)
				{
					addEdge(a, b);
					addEdge(b, c);
					addEdge(c, a);
				}
				a = b;
				b = c;
			}
		}
	};
	// Skip the emulator-generated background quad. Include sprites and all three
	// color lists, including back faces/occluded edges, for a geometry inspection.
	strips(context.global_param_op, std::max(1u, previousPass.op_count), pass.op_count);
	strips(context.global_param_pt, previousPass.pt_count, pass.pt_count);
	strips(context.global_param_tr, previousPass.tr_count, pass.tr_count);
}

void Capture::publish()
{
	if (!frame)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	if (generation == activeGeneration)
		latest = std::move(frame);
}
}
