#include "plugins/geometry/tree/world.h"

#include "plugins/geometry/tree/boxes.h"
#include "plugins/geometry/tree/build.h"
#include "plugins/geometry/tree/lazy.h"
#include "plugins/geometry/tree/pass.h"
#include "sdf/compare.h"

#include <cstdint>
#include <string>
#include <vector>

namespace kodanuki {

std::vector<DetailMember> chosenDetailMembers(const Choices& choices) {
	std::vector<DetailMember> members = { DetailMember::ByDistance };
	for (const std::string& name : choices.owned) {
		if (name.rfind("detail/", 0) == 0) {
			members.push_back(*detailMemberNamed(name.substr(7)));
		}
	}
	return members;
}

std::vector<PassMember> chosenPassMembers(const Choices& choices) {
	std::vector<PassMember> members;
	for (const std::string& name : choices.owned) {
		if (name.rfind("build/", 0) == 0) {
			members.push_back(*passMemberNamed(name.substr(6)));
		}
	}
	return members;
}

BuiltGeometry builtGeometry(const SceneFile& file, const Choices& choices, bool forgiving) {
	const ObjectBoxes boxes                 = objectBoxes(file.scene, choices.threads);
	const Region region                     = choices.framed ? choices.region : boxedRegion(heldBoxes(boxes.boxes, file.scene), file.shot);
	const std::vector<DetailMember> members = chosenDetailMembers(choices);

	const SceneDetailQuery query = {
		.scene   = file.scene,
		.shot    = file.shot,
		.region  = region,
		.members = members,
	};

	Detail detail                        = sceneDetail(query);
	detail.least                         = choices.least > 0 ? choices.least : detail.least;
	const std::vector<PassMember> passes = chosenPassMembers(choices);

	const LazyInput input = {
		.scene     = file.scene,
		.detail    = detail,
		.passes    = passes,
		.boxes     = boxes,
		.region    = region,
		.depthCap  = choices.depth >= 0 ? choices.depth : depthCapOf(region, detail.least),
		.workers   = choices.threads,
		.faces     = choices.faces,
		.memory    = choices.memory > 0 ? std::size_t(choices.memory * 1048576.0) : machineMemory() / 4,
		.forgiving = forgiving,
	};

	return BuiltGeometry {
		.detail = detail,
		.tree   = lazyTree(input),
	};
}

bool keptGeometry(BuiltGeometry& built, const SceneFile& file) {
	if (&built.tree.growth->scene != &file.scene || !sameTileSteps(built.tree.growth->tiles, file.scene)) {
		return false;
	}
	followScene(built.tree, objectKeys(file.scene));
	built.detail = built.tree.growth->detail;
	return true;
}

}
