#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include <r3d/r3d_animation.h>
#include <r3d/r3d_animation_tree.h>

int main(void)
{
    static char idleName[] = "Mechagodzilla_Legendary_skeleton|idle";
    static char walkName[] = "Mechagodzilla_Legendary_skeleton|walk";

    R3D_Animation animations[2] = {
        {.name = idleName, .ticksPerSecond = 1.0f, .duration = 1.0f},
        {.name = walkName, .ticksPerSecond = 1.0f, .duration = 2.0f},
    };
    R3D_AnimationLib library = {
        .animations = animations,
        .count = 2,
    };

    assert(strcmp(library.animations[0].name, idleName) == 0);
    assert(strcmp(library.animations[1].name, walkName) == 0);
    assert(R3D_GetAnimationIndex(library, idleName) == 0);
    assert(R3D_GetAnimationIndex(library, walkName) == 1);

    R3D_AnimationPlayer player = {.animLib = library};
    R3D_AnimationTree tree = R3D_LoadAnimationTree(player, 2);
    assert(tree.nodePool != NULL);

    R3D_AnimationNodeParams parameters = {
        .state = {
            .speed = 1.0f,
            .play = true,
            .loop = true,
        },
        .looper = true,
    };
    R3D_AnimationTreeNode* walk = R3D_CreateAnimationNodeByIndex(&tree, 1, parameters);
    assert(walk != NULL);
    assert(strcmp(R3D_GetAnimationNodeParams(walk).name, walkName) == 0);
    assert(R3D_CreateAnimationNodeByIndex(&tree, -1, parameters) == NULL);
    assert(R3D_CreateAnimationNodeByIndex(&tree, 2, parameters) == NULL);

    R3D_UnloadAnimationTree(tree);
    return 0;
}
