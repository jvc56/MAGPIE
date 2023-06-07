#include <stdlib.h>
#include <stdio.h>

#include "constants.h"
#include "leave_rack.h"

LeaveRack * create_leave_rack(int distribution_size) {
    LeaveRack * leave_rack = malloc(sizeof(LeaveRack));
    leave_rack->draws = 0;
    leave_rack->equity = 0;
    leave_rack->rack = create_rack(distribution_size);
    return leave_rack;
}

void destroy_leave_rack(LeaveRack * leave_rack) {
    destroy_rack(leave_rack->rack);
    free(leave_rack);
}

LeaveRackList * create_leave_rack_list(int capacity, int distribution_size) {
    LeaveRackList *lrl = malloc(sizeof(LeaveRackList));
    lrl->count = 0;
    lrl->capacity = capacity;
    lrl->spare_leave_rack = create_leave_rack(distribution_size);
    lrl->leave_racks = malloc((sizeof(LeaveRack*)) * (lrl->capacity));
    for (int i = 0; i < lrl->capacity; i++) {
        lrl->leave_racks[i] = create_leave_rack(distribution_size);
    }
    return lrl;
}

void destroy_leave_rack_list(LeaveRackList * lrl) {
    for (int i = 0; i < lrl->capacity; i++) {
        destroy_leave_rack(lrl->leave_racks[i]);
    }
    destroy_leave_rack(lrl->spare_leave_rack);
    free(lrl->leave_racks);
    free(lrl);
}

void reset_leave_rack_list(LeaveRackList * lrl) {
    lrl->count = 0;
    lrl->leave_racks[0]->draws = 0;
}

void up_heapify_leave_rack(LeaveRackList * lrl, int index){
    LeaveRack * temp;
    int parent_node = (index-1)/2;

    if(lrl->leave_racks[parent_node]->draws > lrl->leave_racks[index]->draws){
        temp = lrl->leave_racks[parent_node];
        lrl->leave_racks[parent_node] = lrl->leave_racks[index];
        lrl->leave_racks[index] = temp;
        up_heapify_leave_rack(lrl,parent_node);
    }
}

void down_heapify_leave_rack(LeaveRackList * lrl, int parent_node){
    int left = parent_node*2+1;
    int right = parent_node*2+2;
    int min;
    LeaveRack * temp;

    if(left >= lrl->count || left <0)
        left = -1;
    if(right >= lrl->count || right <0)
        right = -1;

    if(left != -1 && lrl->leave_racks[left]->draws < lrl->leave_racks[parent_node]->draws)
        min = left;
    else
        min = parent_node;
    if(right != -1 && lrl->leave_racks[right]->draws < lrl->leave_racks[min]->draws)
        min = right;

    if(min != parent_node){
        temp = lrl->leave_racks[min];
        lrl->leave_racks[min] = lrl->leave_racks[parent_node];
        lrl->leave_racks[parent_node] = temp;
        down_heapify_leave_rack(lrl, min);
    }
}

void insert_leave_rack(LeaveRackList * lrl, Rack * rack, int number_of_draws_for_leave, double equity) {
    for (int i = 0; i < rack->array_size; i++) {
        lrl->spare_leave_rack->rack->array[i] = rack->array[i];
    }
    lrl->spare_leave_rack->draws = number_of_draws_for_leave;
    lrl->spare_leave_rack->equity = equity;

    LeaveRack * swap = lrl->leave_racks[lrl->count];
    lrl->leave_racks[lrl->count] = lrl->spare_leave_rack;
    lrl->spare_leave_rack = swap;

    up_heapify_leave_rack(lrl, lrl->count);
    lrl->count++;

    if (lrl->count > lrl->capacity) {
        pop_leave_rack(lrl);
    }
}

LeaveRack * pop_leave_rack(LeaveRackList * lrl) {
    if (lrl->count == 1) {
        lrl->count--;
        return lrl->leave_racks[0];
    }
    LeaveRack * swap = lrl->spare_leave_rack;
    lrl->spare_leave_rack = lrl->leave_racks[0];
    lrl->leave_racks[0] = lrl->leave_racks[lrl->count-1];
    lrl->leave_racks[lrl->count-1] = swap;

    lrl->count--;
    down_heapify_leave_rack(lrl, 0);
    return lrl->spare_leave_rack;
}
