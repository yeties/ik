#include "ik/joblist.h"

#include "cstructures/memory.h"
#include "cstructures/vector.h"
#include "ik/effector.h"
#include "ik/log.h"
#include "ik/node.h"
#include "ik/solver.h"
#include "ik/subtree.h"
#include <stddef.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

enum ik_marking
{
    MARK_SECTION,
    MARK_BEGIN,
    MARK_END,
    MARK_BEGIN_AND_END
};

/* ------------------------------------------------------------------------- */
static int
find_all_effector_nodes(struct vector_t* result, const struct ik_node* node)
{
    NODE_FOR_EACH(node, user_data, child)
        if (find_all_effector_nodes(result, child) != 0)
            return -1;
    NODE_END_EACH

    if (node->effector != NULL)
        if (vector_push(result, &node) != VECTOR_OK)
            return -1;

    return 0;
}

/* ------------------------------------------------------------------------- */
static int
mark_nodes(struct btree_t* marked, const struct vector_t* effector_nodes)
{
    /*
     * Iterate the chain of nodes starting at each effector node and ending
     * at the specified chain length of the effector, mark every node on the
     * way.
     */
    VECTOR_FOR_EACH(effector_nodes, const struct ik_node*, p_effector_node)
        const struct ik_node* node = *p_effector_node;
        const struct ik_effector* effector = node->effector;
        int chain_length_counter = (int)effector->chain_length != 0 ?
                                   (int)effector->chain_length : -1;

        /*
         * Walk up chain starting at effector node and ending if we run out of
         * nodes, or the chain length counter reaches 0. Mark every node in
         * the chain as MARK_SECTION. If we get to the last node in the chain,
         * mark it as MARK_END only if the node is unmarked. This means that
         * nodes marked as MARK_END will be overwritten with MARK_SECTION if
         * necessary.
         */
        while (1)
        {
#define is_end_of_chain() \
            (chain_length_counter == 0 || node->parent == NULL)
#define has_children() \
            (ik_node_child_count(node) > 0)
#define has_algorithm() \
            (node->algorithm != NULL)
#define has_effector() \
            (node->effector != NULL)

            enum ik_marking* existing_mark;
            const enum ik_marking lookup_mark[16] = {
                -1,
                -1,
                MARK_SECTION,
                MARK_BEGIN,
                MARK_END,
                MARK_END,
                MARK_BEGIN_AND_END,
                MARK_BEGIN_AND_END,
                -1,
                -1,
                MARK_SECTION,
                MARK_BEGIN,
                MARK_BEGIN,
                MARK_BEGIN_AND_END,
                MARK_BEGIN_AND_END,
                MARK_BEGIN_AND_END
            };

            const uint8_t mark_idx =
                (is_end_of_chain() << 0) |
                (has_children()    << 1) |
                (has_effector()    << 2) |
                (has_algorithm()   << 3);

            enum ik_marking new_mark = lookup_mark[mark_idx];

            switch (mark_idx) {
                case 0: case 1: case 8: case 9: {
                    ik_log_printf(IK_FATAL, "mark_nodes(): Found a leaf node with no effector attached. This should never happen!");
                    return -1;
                } break;

                case 10: case 11: case 12: {
                    ik_log_printf(IK_WARN, "Attached algorithm on node %zu (0x%p) is useless", node->user.guid, node->user.ptr);
                } break;

                default: break;
            }

            switch (btree_insert_or_get(marked, node->user.guid, &new_mark, (void**)&existing_mark))
            {
                case BTREE_EXISTS: {
                    /* overwrite existing mark with MARK_SECTION */
                    if (new_mark == MARK_SECTION)
                        *existing_mark = MARK_SECTION;
                } break;

                case BTREE_NOT_FOUND: {
                    /* mark was inserted */
                } break;

                default: {
                    ik_log_out_of_memory("btree_insert_or_get()");
                    return -1;
                }
            }

            if (chain_length_counter == 0 || node->parent == NULL)
                break;
        }
    VECTOR_END_EACH

    return 0;
}

/* ------------------------------------------------------------------------- */
static struct ik_solver*
new_solver(struct ik_subtree* subtree)
{
    const struct ik_node* node;
    const struct ik_algorithm* algorithm;
    struct ik_solver* solver;

    algorithm = NULL;
    for (node = subtree->root; node->parent != NULL; node = node->parent)
        if (node->algorithm != NULL)
        {
            algorithm = node->algorithm;
            break;
        }
    if (algorithm == NULL)
    {
        ik_log_printf(IK_ERROR, "No algorithm assigned to subtree starting at node %zu (0x%p)",
                      subtree->root->user.guid,
                      subtree->root->user.ptr);
        return NULL;
    }

    solver = ik_solver_create(subtree, algorithm);
    if (solver == NULL)
        return NULL;

    return solver;
}

/* ------------------------------------------------------------------------- */
static int
alloc_solvers(struct ik_joblist* joblist,
              struct ik_subtree* current_subtree,
              const struct ik_node* node,
              const struct btree_t* marked_nodes);
static int
recurse_with_new_subtree(struct ik_joblist* joblist,
              const struct ik_node* node,
              const struct btree_t* marked_nodes)
{
    struct ik_solver* solver;
    struct ik_subtree subtree;

    if (ik_subtree_init(&subtree) != 0)
        goto subtree_init_failed;

    subtree.root = node;
    NODE_FOR_EACH(node, user_data, child)
        if (alloc_solvers(joblist, &subtree, child, marked_nodes) != 0)
            goto recurse_failed;
    NODE_END_EACH

    solver = new_solver(&subtree);
    if (solver == NULL)
        goto new_solver_failed;

    if (vector_push(&joblist->solvers, &solver) != VECTOR_OK)
        goto add_solver_to_joblist_failed;

    ik_subtree_deinit(&subtree);

    return 0;

    add_solver_to_joblist_failed : ik_solver_free(solver);
    new_solver_failed            :
    recurse_failed               : ik_subtree_deinit(&subtree);
    subtree_init_failed          : return -1;
}

/* ------------------------------------------------------------------------- */
static int
alloc_solvers(struct ik_joblist* joblist,
              struct ik_subtree* current_subtree,
              const struct ik_node* node,
              const struct btree_t* marked_nodes)
{
    enum ik_marking* mark = btree_find(marked_nodes, node->user.guid);
    if (mark) switch(*mark)
    {
        case MARK_END:
            assert(current_subtree != NULL);
            if (vector_push(&current_subtree->leaves, &node) != VECTOR_OK)
                return -1;
        /* fallthrough */

        case MARK_SECTION:
            NODE_FOR_EACH(node, user_data, child)
                if (alloc_solvers(joblist, current_subtree, child, marked_nodes) != 0)
                    return -1;
            NODE_END_EACH
            break;

        case MARK_BEGIN_AND_END:
            assert(current_subtree != NULL);
            if (vector_push(&current_subtree->leaves, &node) != VECTOR_OK)
                return -1;
        /* fallthrough */

        case MARK_BEGIN:
            return recurse_with_new_subtree(joblist, node, marked_nodes);
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
static void
deinit_joblist(struct ik_joblist* joblist)
{
    VECTOR_FOR_EACH(&joblist->solvers, struct ik_solver*, p_solver)
        ik_solver_free(*p_solver);
    VECTOR_END_EACH
    vector_deinit(&joblist->solvers);
}

/* ------------------------------------------------------------------------- */
struct ik_joblist*
ik_joblist_create(const struct ik_node* root)
{
    struct ik_joblist* joblist = (struct ik_joblist*)
        ik_refcounted_alloc(sizeof *joblist, (ik_deinit_func)deinit_joblist);
    if (joblist == NULL)
        goto alloc_joblist_failed;

    if (vector_init(&joblist->solvers, sizeof(struct ik_solver*)) != VECTOR_OK)
    {
        ik_log_out_of_memory("vector_init()");
        goto init_solver_vector_failed;
    }

    if (ik_joblist_update(joblist, root) != IK_OK)
    {
        IK_DECREF(joblist);
        return NULL;
    }

    return joblist;

    init_solver_vector_failed : ik_refcounted_free((struct ik_refcounted*)joblist);
    alloc_joblist_failed      : return NULL;
}

/* ------------------------------------------------------------------------- */
ikret
ik_joblist_update(struct ik_joblist* joblist, const struct ik_node* root)
{
    ikret status;
    struct vector_t effector_nodes;
    struct btree_t marked_nodes;

    /* Create a list of all nodes that have effectors attached */
    if (vector_init(&effector_nodes, sizeof(struct ik_node*)) != VECTOR_OK)
    {
        ik_log_out_of_memory("vector_init()");
        status = IK_ERR_OUT_OF_MEMORY;
        goto init_effector_nodes_failed;
    }
    if ((status = find_all_effector_nodes(&effector_nodes, root)) != IK_OK)
        goto find_effectors_failed;

    /* May not have to do anything */
    if (vector_count(&effector_nodes) == 0)
    {
        ik_log_printf(IK_WARN, "No effectors were found in the tree. Joblist is empty.");
        status = IK_ERR_NO_EFFECTORS_FOUND;
        goto find_effectors_failed;
    }

    /* Mark all nodes that the effectors can reach */
    if (btree_init(&marked_nodes, sizeof(enum ik_marking)) != BTREE_OK)
    {
        ik_log_out_of_memory("btree_init()");
        status = IK_ERR_OUT_OF_MEMORY;
        goto init_marked_nodes_failed;
    }
    if ((status = mark_nodes(&marked_nodes, &effector_nodes)) != IK_OK)
        goto mark_nodes_failed;

    /* clear old solvers */
    VECTOR_FOR_EACH(&joblist->solvers, struct ik_solver*, p_solver)
        ik_solver_free(*p_solver);
    VECTOR_END_EACH
    vector_clear_compact(&joblist->solvers);

    /*
     * It's possible that chain length limits end up isolating parts of the
     * tree, splitting it into a list of "sub-trees" which must be solved
     * in-order.
     */
    if ((status = alloc_solvers(joblist, NULL, root, &marked_nodes)) != IK_OK)
        goto split_into_subtrees_failed;

    btree_deinit(&marked_nodes);
    vector_deinit(&effector_nodes);

    return IK_OK;

    split_into_subtrees_failed     :
    mark_nodes_failed              : btree_deinit(&marked_nodes);
    init_marked_nodes_failed       :
    find_effectors_failed          : vector_deinit(&effector_nodes);
    init_effector_nodes_failed     : return status;
}