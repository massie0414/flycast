#pragma once

#include "types.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

struct rend_context;
struct RenderPass;

namespace polygonPreview
{
constexpr u32 MaxTriangles = 100000;

struct Edge
{
	glm::vec2 a, b; // Clipped to [0, 1], with Y pointing down.
};

struct Frame
{
	float aspectRatio = 4.f / 3.f;
	u32 triangles = 0;
	bool limited = false;
	std::vector<Edge> edges;
};

void setEnabled(bool enabled);
void clear();
std::shared_ptr<const Frame> snapshot();

// Capture original strips before sorting, merging and primitive-restart indices.
// The viewer receives immutable CPU data and never accesses renderer resources.
class Capture
{
public:
	explicit Capture(const rend_context& context);
	void addPass(const RenderPass& pass, const RenderPass& previousPass);
	void publish();

private:
	void addEdge(glm::vec3 a, glm::vec3 b);
	const rend_context& context;
	std::shared_ptr<Frame> frame;
	glm::mat4 screenMatrix{1.f};
	u64 generation = 0;
	bool rotate = false;
};
}
