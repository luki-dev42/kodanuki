#include "plugins/geometry/tree/pass.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace kodanuki {

PassMoment passMoment(PassMember member) {
	switch (member) {
		case PassMember::CutLongPrograms:
		case PassMember::FaceSlabs: return PassMoment::Grown;
		case PassMember::SharedCoefficients: return PassMoment::Placed;
	}

	return PassMoment::Grown;
}

Subtree passSubtree(PassMember member, const PassQuery& query, PassState& state) {
	switch (member) {
		case PassMember::CutLongPrograms: return cutLongPrograms(query.context, query.subtree, query.programs);
		case PassMember::FaceSlabs: return faceSlabs(query.subtree);
		case PassMember::SharedCoefficients: return sharedCoefficients(query.tree, query.subtree, state.coefficients);
	}

	return query.subtree;
}

PassState emptyPassState() {
	return PassState {
		.coefficients = emptySharedRuns(),
	};
}

SlabLeg passLeg(const RunHeader& header, const float* base, const float* rate) {
	if (!faceSlabsBuilt || !header.slabbed) {
		return unheldSlabLeg();
	}
	return slabLegOf(base, rate);
}

int passFaceSide(const SlabLeg& leg, const std::uint32_t* slab) {
	if (!faceSlabsBuilt) {
		return 0;
	}
	return slabSide(leg, slab);
}

std::vector<std::uint32_t> passRunWords(const RunQuery& query) {
	return facedWords(query);
}

std::vector<std::uint32_t> passFoldedWords(const std::uint32_t* words, std::uint32_t count, std::uint32_t kept) {
	return foldedFacedWords(words, count, kept);
}

LandedFace passLandedFace(const std::uint32_t* run, int face, const float* unit) {
	LandedFace landed = {
		.exact    = false,
		.gradient = { 0, 0, 0 },
		.leaf     = noLeaf,
		.painter  = 0,
		.owner    = noFaceOwner,
	};

	if (!facedHitsBuilt || face < 0) {
		return landed;
	}
	const FacedFace known = facedFace(run, std::uint32_t(face));
	landed.exact          = known.exact;
	landed.leaf           = known.leaf;
	landed.painter        = known.painter;
	landed.owner          = known.owner;
	if (known.exact && unit != nullptr) {
		landed.gradient = facedGradient(run, std::uint32_t(face), unit);
	}
	return landed;
}

bool passReadsCellPast() {
	return facedHitsBuilt;
}

std::vector<PassMember> defaultPassMembers() {
	return { PassMember::CutLongPrograms, PassMember::FaceSlabs, PassMember::SharedCoefficients };
}

std::optional<PassMember> passMemberNamed(const std::string& name) {
	const std::array<std::string, 3> names = { "cut_long_programs", "face_slabs", "shared_coefficients" };
	for (std::size_t k = 0; k < names.size(); ++k) {
		if (names[k] == name) {
			return PassMember(k);
		}
	}
	return std::nullopt;
}

}
