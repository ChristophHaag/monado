// Copyright 2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Main code for kinematic model
 * @author Moses Turner <moses@collabora.com>
 * @ingroup tracking
 */

#include "kinematic_defines.hpp"
#include "kinematic_tiny_math.hpp"
#include "kinematic_hand_init.hpp"

#include <Eigen/Core>
#include <Eigen/LU>
#include <Eigen/SVD>
#include <Eigen/src/Geometry/Umeyama.h>



namespace xrt::tracking::hand::mercury::kine {

static void
_two_set_ele(Eigen::Matrix<float, 3, 21> &thing, Eigen::Affine3f jt, int idx)
{
	// slow
	thing.col(idx) = jt.translation();
}

static void
two(struct kinematic_hand_4f *hand)
{
	XRT_TRACE_MARKER();


	int i = 0;

	_two_set_ele(hand->kinematic, hand->wrist_relation, i);

	for (int finger_idx = 0; finger_idx < 5; finger_idx++) {
		for (int bone_idx = 1; bone_idx < 5; bone_idx++) {

			i++;

			_two_set_ele(hand->kinematic, hand->fingers[finger_idx].bones[bone_idx].world_pose, i);
		}
	}


	Eigen::Affine3f aff3d;

	aff3d = Eigen::umeyama(hand->kinematic, hand->t_jts_as_mat, false);

	hand->wrist_relation = aff3d * hand->wrist_relation;

	_statics_init_world_poses(hand);
}

#if 0
#define DOT_EPSILON 0.000001f

Eigen::Quaternionf
fast_simple_rotation(const Eigen::Vector3f &from_un, const Eigen::Vector3f &to_un)
{
	// Slooowwwwwww....
	Eigen::Vector3f from = from_un.normalized();
	Eigen::Vector3f to = to_un.normalized();

	Eigen::Vector3f axis = from.cross(to);
	float dot = from.dot(to);

	if (dot < (-1.0f + DOT_EPSILON)) {
		return Eigen::Quaternionf(0, 1, 0, 0);
	};

	Eigen::Quaternionf result(axis.x() * 0.5f, axis.y() * 0.5f, axis.z() * 0.5f, (dot + 1.0f) * 0.5f);
	result.normalize();
	return result;
}

Eigen::Matrix3f
moses_fast_simple_rotation(const Eigen::Vector3f &from_un, const Eigen::Vector3f &to_un)
{
	// Slooowwwwwww....
	Eigen::Vector3f from = from_un.normalized();
	Eigen::Vector3f to = to_un.normalized();

	Eigen::Vector3f axis = from.cross(to).normalized();
	float angle = acos(from.dot(to));

	// U_LOG_E("HERE %f", angle);
	// std::cout << to << std::endl;
	// std::cout << from << std::endl;
	// std::cout << axis << std::endl;
	// std::cout << angle << std::endl;

	Eigen::AngleAxisf ax;

	ax.axis() = axis;
	ax.angle() = angle;

	return ax.matrix();
}
#endif


static void
do_it_for_bone(struct kinematic_hand_4f *hand, int finger_idx, int bone_idx, bool clamp_to_x_axis_rotation)
{
	finger_t *of = &hand->fingers[finger_idx];
	bone_t *bone = &hand->fingers[finger_idx].bones[bone_idx];
	int num_children = 0;

	Eigen::Vector3f kine = Eigen::Vector3f::Zero();
	Eigen::Vector3f target = Eigen::Vector3f::Zero();


	for (int idx = bone_idx + 1; idx < 5; idx++) {
		num_children++;
		target += map_vec3(hand->t_jts[of->bones[idx].keypoint_idx_21]);
		kine += of->bones[idx].world_pose.translation();
	}

	kine *= 1.0f / (float)num_children;
	target *= 1.0f / (float)num_children;

	kine = bone->world_pose.inverse() * kine;
	target = bone->world_pose.inverse() * target;

	kine.normalize();
	target.normalize();

	Eigen::Matrix3f rot = Eigen::Quaternionf().setFromTwoVectors(kine, target).matrix();

	bone->bone_relation.linear() = bone->bone_relation.linear() * rot;
}

#if 1
static void
clamp_to_x_axis(struct kinematic_hand_4f *hand,
                int finger_idx,
                int bone_idx,
                bool clamp_angle = false,
                float min_angle = 0,
                float max_angle = 0)
{
	bone_t &bone = hand->fingers[finger_idx].bones[bone_idx];
	// U_LOG_E("before_anything");

	// std::cout << bone->bone_relation.linear().matrix() << std::endl;
	int axis = 0;
	Eigen::Vector3f axes[3] = {Eigen::Vector3f::UnitX(), Eigen::Vector3f::UnitY(), Eigen::Vector3f::UnitZ()};


	// The input rotation will very likely rotate a vector pointed in the direction of axis we want to lock to...
	// somewhere else. So, we find the new direction
	Eigen::Vector3f axis_rotated_by_input = bone.bone_relation.linear() * axes[axis];

	// And find a correction rotation that rotates our input rotation such that it doesn't affect vectors pointing
	// in the direction of our axis anymore.
	Eigen::Matrix3f correction_rot =
	    Eigen::Quaternionf().setFromTwoVectors(axis_rotated_by_input.normalized(), axes[axis]).matrix();
	bone.bone_relation.linear() = correction_rot * bone.bone_relation.linear();


	if (!clamp_angle) {
		return;
	}

	Eigen::Vector3f axis_to_clamp_rotation = axes[(axis + 1) % 3];

	Eigen::Vector3f new_ortho_direction = bone.bone_relation.linear() * axis_to_clamp_rotation;
	float rotation_value = atan2(new_ortho_direction((axis + 2) % 3), new_ortho_direction((axis + 1) % 3));



	if (rotation_value < max_angle && rotation_value > min_angle) {
		return;
	}

	float positive_rotation_value, negative_rotation_value;


	if (rotation_value < 0) {
		positive_rotation_value = (M_PI * 2) - rotation_value;
		negative_rotation_value = rotation_value;
	} else {
		negative_rotation_value = (-M_PI * 2) + rotation_value;
		positive_rotation_value = rotation_value;
	}

	if ((positive_rotation_value - max_angle) > (min_angle - negative_rotation_value)) {
		// Difference between max angle and positive value is higher, so we're closer to the minimum bound.
		rotation_value = min_angle;
	} else {
		rotation_value = max_angle;
	}

	bone.bone_relation.linear() = Eigen::AngleAxisf(rotation_value, axes[axis]).toRotationMatrix();
}

#else
static void
clamp_to_x_axis(struct kinematic_hand_4f *hand,
                int finger_idx,
                int bone_idx,
                bool clamp_angle = false,
                float min_angle = 0,
                float max_angle = 0)
{
	bone_t *bone = &hand->fingers[finger_idx].bones[bone_idx];
	// U_LOG_E("before_anything");

	// std::cout << bone->bone_relation.linear().matrix() << std::endl;



	Eigen::Vector3f new_x = bone->bone_relation.linear() * Eigen::Vector3f::UnitX();
	Eigen::Matrix3f correction_rot =
	    Eigen::Quaternionf().setFromTwoVectors(new_x.normalized(), Eigen::Vector3f::UnitX()).matrix();
	// U_LOG_E("correction");

	// std::cout << correction_rot << std::endl;

	// U_LOG_E("correction times new_x");
	// std::cout << correction_rot * new_x << "\n";

	// U_LOG_E("before, where does relation point x");
	// std::cout << bone->bone_relation.rotation() * Eigen::Vector3f::UnitX() << "\n";


	// Weird that we're left-multiplying here, I don't know why. But it works.
	bone->bone_relation.linear() = correction_rot * bone->bone_relation.linear();

	// U_LOG_E("after_correction");


	// std::cout << bone->bone_relation.linear() << std::endl;


	// U_LOG_E("after, where does relation point x");
	// std::cout << bone->bone_relation.linear() * Eigen::Vector3f::UnitX() << "\n";

	if (clamp_angle) {
		//! @todo optimize: get rid of 1 and 2, we only need 0.

		// signed angle: asin(Cross product of -z and rot*-z X axis.
		// U_LOG_E("before X clamp");
		// std::cout << bone->bone_relation.linear() << "\n";

		// auto euler = bone->bone_relation.linear().eulerAngles(0, 1, 2);

		Eigen::Vector3f cross =
		    (-Eigen::Vector3f::UnitZ()).cross(bone->bone_relation.linear() * (-Eigen::Vector3f::UnitZ()));

		// std::cout << bone->bone_relation.linear() << "\n";


		// U_LOG_E("eluer before clamp: %f %f  %f %f %f", min_angle, max_angle, euler(0), euler(1), euler(2));
		// U_LOG_E("eluer before clamp: %f %f %f  %f %f %f", euler(0), euler(1), euler(2), cross(0), cross(1) ,
		// cross(2));



		//! @todo optimize: Move the asin into constexpr land
		// No, the sine of the joint limit
		float rotation_value = asin(cross(0));

		//!@bug No. If the rotation value is outside of the allowed values, clamp it to the rotation it's
		//!*closest to*.
		// That's different than using clamp, rotation formalisms are complicated.
		clamp(&rotation_value, min_angle, max_angle);

		// U_LOG_E("eluer after clamp: %f", rotation_value);


		Eigen::Matrix3f n;
		n = Eigen::AngleAxisf(rotation_value, Eigen::Vector3f::UnitX());
		bone->bone_relation.linear() = n;
		// U_LOG_E("after X clamp");

		// std::cout << n << "\n";
	}
}
#endif


// Is this not just swing-twist about the Z axis? Dunnoooo... Find out later.
static void
clamp_proximals(struct kinematic_hand_4f *hand,
                int finger_idx,
                int bone_idx,
                float max_swing_angle = 0,
                float tanangle_left = tan(rad(-20)),
                float tanangle_right = tan(rad(20)),
                float tanangle_curled = tan(rad(-89)), // Uh oh...
                float tanangle_uncurled = tan(rad(30)))
{
	bone_t *bone = &hand->fingers[finger_idx].bones[bone_idx];


	Eigen::Matrix3f rot = bone->bone_relation.linear();

	// U_LOG_E("rot");
	// std::cout << rot << "\n";

	Eigen::Vector3f our_z = rot * (-Eigen::Vector3f::UnitZ());

	Eigen::Matrix3f simple = Eigen::Quaternionf().setFromTwoVectors(-Eigen::Vector3f::UnitZ(), our_z).matrix();
	// U_LOG_E("simple");
	// std::cout << simple << "\n";

	Eigen::Matrix3f twist = rot * simple.inverse();
	// U_LOG_E("twist");

	// std::cout << twist << "\n";

	// U_LOG_E("twist_axis");

	Eigen::AngleAxisf twist_aax = Eigen::AngleAxisf(twist);

	// std::cout << twist_aax.axis() << "\n";

	// U_LOG_E("twist_angle");

	// std::cout << twist_aax.angle() << "\n";



	// U_LOG_E("all together now");

	// std::cout << twist * simple << "\n";

	if (fabs(twist_aax.angle()) > max_swing_angle) {
		// max_swing_angle times +1 or -1, depending.
		twist_aax.angle() = max_swing_angle * (twist_aax.angle() / fabs(twist_aax.angle()));
		// U_LOG_E("clamping twist %f", twist_aax.angle());


		// std::cout << twist << "\n";
		// std::cout << twist_aax.toRotationMatrix() << "\n";
	}


	if (our_z.z() > 0) {
		//!@bug We need smarter joint limiting, limiting using tanangles is not acceptable as joints can rotate
		//! outside of the 180 degree hemisphere.
		our_z.z() = -0.000001f;
	}
	our_z *= -1.0f / our_z.z();


	clamp(&our_z.x(), tanangle_left, tanangle_right);
	clamp(&our_z.y(), tanangle_curled, tanangle_uncurled);

	simple = Eigen::Quaternionf().setFromTwoVectors(-Eigen::Vector3f::UnitZ(), our_z.normalized()).matrix();

	bone->bone_relation.linear() = twist_aax * simple;
}



static void
do_it_for_finger(struct kinematic_hand_4f *hand, int finger_idx)
{
	do_it_for_bone(hand, finger_idx, 0, false);
	clamp_proximals(hand, finger_idx, 0, rad(4.0), tan(rad(-30)), tan(rad(30)), tan(rad(-10)), tan(rad(10)));
	_statics_init_world_poses(hand);

	do_it_for_bone(hand, finger_idx, 1, true);
	clamp_proximals(hand, finger_idx, 1, rad(4.0));
	_statics_init_world_poses(hand);


	do_it_for_bone(hand, finger_idx, 2, true);
	clamp_to_x_axis(hand, finger_idx, 2, true, rad(-90), rad(10));
	_statics_init_world_poses(hand);

	do_it_for_bone(hand, finger_idx, 3, true);
	clamp_to_x_axis(hand, finger_idx, 3, true, rad(-90), rad(10));
	_statics_init_world_poses(hand);
}

static void
optimize(kinematic_hand_4f *hand)
{
	for (int i = 0; i < 15; i++) {
		two(hand);
		do_it_for_bone(hand, 0, 1, false);
		clamp_proximals(hand, 0, 1, rad(70), tan(rad(-40)), tan(rad(40)), tan(rad(-40)), tan(rad(40)));
		_statics_init_world_poses(hand);

		do_it_for_bone(hand, 0, 2, true);
		clamp_to_x_axis(hand, 0, 2, true, rad(-90), rad(40));
		_statics_init_world_poses(hand);

		do_it_for_bone(hand, 0, 3, true);
		clamp_to_x_axis(hand, 0, 3, true, rad(-90), rad(40));
		_statics_init_world_poses(hand);

		two(hand);

		do_it_for_finger(hand, 1);
		do_it_for_finger(hand, 2);
		do_it_for_finger(hand, 3);
		do_it_for_finger(hand, 4);
	}
	two(hand);
}


static void
make_joint_at_matrix_left_hand(int idx, Eigen::Affine3f &pose, struct xrt_hand_joint_set *hand)
{
	hand->values.hand_joint_set_default[idx].relation.relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	    XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
	Eigen::Vector3f v = pose.translation();
	hand->values.hand_joint_set_default[idx].relation.pose.position.x = v.x();
	hand->values.hand_joint_set_default[idx].relation.pose.position.y = v.y();
	hand->values.hand_joint_set_default[idx].relation.pose.position.z = v.z();

	Eigen::Quaternionf q;
	q = pose.rotation();

	hand->values.hand_joint_set_default[idx].relation.pose.orientation.x = q.x();
	hand->values.hand_joint_set_default[idx].relation.pose.orientation.y = q.y();
	hand->values.hand_joint_set_default[idx].relation.pose.orientation.z = q.z();
	hand->values.hand_joint_set_default[idx].relation.pose.orientation.w = q.w();
}

static void
make_joint_at_matrix_right_hand(int idx, Eigen::Affine3f &pose, struct xrt_hand_joint_set *hand)
{
	hand->values.hand_joint_set_default[idx].relation.relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	    XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
	Eigen::Vector3f v = pose.translation();
	hand->values.hand_joint_set_default[idx].relation.pose.position.x = -v.x();
	hand->values.hand_joint_set_default[idx].relation.pose.position.y = v.y();
	hand->values.hand_joint_set_default[idx].relation.pose.position.z = v.z();

	Eigen::Matrix3f rotation = pose.rotation();

	Eigen::Matrix3f mirror_on_x = Eigen::Matrix3f::Identity();
	mirror_on_x(0, 0) = -1;

	rotation = mirror_on_x * rotation;

	rotation(0, 0) *= -1;
	rotation(1, 0) *= -1;
	rotation(2, 0) *= -1;

	Eigen::Quaternionf q;

	q = rotation;

	hand->values.hand_joint_set_default[idx].relation.pose.orientation.x = q.x();
	hand->values.hand_joint_set_default[idx].relation.pose.orientation.y = q.y();
	hand->values.hand_joint_set_default[idx].relation.pose.orientation.z = q.z();
	hand->values.hand_joint_set_default[idx].relation.pose.orientation.w = q.w();
}

static void
make_joint_at_matrix(int idx, Eigen::Affine3f &pose, struct xrt_hand_joint_set *hand, int hand_idx)
{
	if (hand_idx == 0) {
		make_joint_at_matrix_left_hand(idx, pose, hand);
	} else {
		make_joint_at_matrix_right_hand(idx, pose, hand);
	}
}

// Exported:
void
optimize_new_frame(xrt_vec3 model_joints_3d[21],
                   kinematic_hand_4f *hand,
                   struct xrt_hand_joint_set *out_set,
                   int hand_idx)
{
	// intake poses!
	for (int i = 0; i < 21; i++) {
		if (hand_idx == 0) {
			hand->t_jts[i] = model_joints_3d[i];
		} else {
			hand->t_jts[i].x = -model_joints_3d[i].x;
			hand->t_jts[i].y = model_joints_3d[i].y;
			hand->t_jts[i].z = model_joints_3d[i].z;
		}

		hand->t_jts_as_mat(0, i) = hand->t_jts[i].x;
		hand->t_jts_as_mat(1, i) = hand->t_jts[i].y;
		hand->t_jts_as_mat(2, i) = hand->t_jts[i].z;
	}

	// do the math!
	optimize(hand);

	// Convert it to xrt_hand_joint_set!

	make_joint_at_matrix(XRT_HAND_JOINT_WRIST, hand->wrist_relation, out_set, hand_idx);

	Eigen::Affine3f palm_relation;

	palm_relation.linear() = hand->fingers[2].bones[0].world_pose.linear();

	palm_relation.translation() = Eigen::Vector3f::Zero();
	palm_relation.translation() += hand->fingers[2].bones[0].world_pose.translation() / 2;
	palm_relation.translation() += hand->fingers[2].bones[1].world_pose.translation() / 2;

	make_joint_at_matrix(XRT_HAND_JOINT_PALM, palm_relation, out_set, hand_idx);

	int start = XRT_HAND_JOINT_THUMB_METACARPAL;

	for (int finger_idx = 0; finger_idx < 5; finger_idx++) {

		for (int bone_idx = 0; bone_idx < 5; bone_idx++) {
			CONTINUE_IF_HIDDEN_THUMB;

			if (!(finger_idx == 0 && bone_idx == 0)) {
				make_joint_at_matrix(start++, hand->fingers[finger_idx].bones[bone_idx].world_pose,
				                     out_set, hand_idx);
			}
		}
	}

	out_set->is_active = true;
}

void
alloc_kinematic_hand(kinematic_hand_4f **out_kinematic_hand)
{
	kinematic_hand_4f *h = new kinematic_hand_4f;
	*out_kinematic_hand = h;
}

void
free_kinematic_hand(kinematic_hand_4f **kinematic_hand)
{
	delete *kinematic_hand;
}

} // namespace xrt::tracking::hand::mercury::kine
