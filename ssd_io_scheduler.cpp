/*
 * ssd_io_scheduler.cpp
 *
 *  Created on: Apr 15, 2012
 *      Author: niv
 */

#include "ssd.h"
#include <limits>

using namespace ssd;

IOScheduler::IOScheduler(Ssd& ssd) :
	ssd(ssd)
{}

IOScheduler::~IOScheduler(){}

IOScheduler *IOScheduler::inst = NULL;

void IOScheduler::instance_initialize(Ssd& ssd)
{
	IOScheduler::inst = new IOScheduler(ssd);
}

IOScheduler *IOScheduler::instance()
{
	return IOScheduler::inst;
}

bool myfunction (const Event& i, const Event& j) {
	return i.get_start_time() + i.get_time_taken() > j.get_start_time() + j.get_time_taken();
}

bool bus_wait_time_comparator (const Event& i, const Event& j) {
	return i.get_bus_wait_time() > j.get_bus_wait_time();
}

void IOScheduler::schedule_independent_event(Event& event) {
	io_schedule.push_back(event);
	std::sort(io_schedule.begin(), io_schedule.end(), myfunction);

	//printf("top id: %d   start time: %f\n", io_schedule.back().get_id(), io_schedule.back().get_start_time());
	//printf("new id: %d   start time: %f\n", event.get_id(), event.get_start_time());

	while (io_schedule.back().get_start_time() + 1 <= event.get_start_time()) {
		execute_current_waiting_ios();
	}
}

void IOScheduler::schedule_dependency(Event& event) {
	int application_io_id = event.get_application_io_id();

	if (event.get_event_type() == READ) {
		event.set_event_type(READ_TRANSFER);
		Event* read_command = new Event(READ_COMMAND, event.get_logical_address(), event.get_size(), event.get_start_time());
		read_command->set_address(event.get_address());
		read_command->set_application_io_id(application_io_id);
		dependencies[application_io_id].push(*read_command);
	}
	dependencies[application_io_id].push(event);
}

void IOScheduler::launch_dependency(uint application_io_id) {
	Event first = dependencies[application_io_id].front();
	dependencies[application_io_id].pop();
	schedule_independent_event(first);
}

void IOScheduler::finish() {
	while (io_schedule.size() > 0) {
		execute_current_waiting_ios();
	}
}

std::vector<Event> IOScheduler::gather_current_waiting_ios() {
	Event top = io_schedule.back();
	io_schedule.pop_back();
	double start_time = top.get_start_time();
	std::vector<Event> current_ios;
	current_ios.push_back(top);
	while (io_schedule.size() > 0 && start_time + 1 > io_schedule.back().get_start_time()) {
		Event current_top = io_schedule.back();
		io_schedule.pop_back();
		current_ios.push_back(current_top);
	}
	return current_ios;
}

void IOScheduler::execute_current_waiting_ios() {
	std::vector<Event> current_ios = gather_current_waiting_ios();
	std::vector<Event> overdue_events;
	std::vector<Event> read_commands;
	std::vector<Event> read_transfers;
	std::vector<Event> writes;
	std::vector<Event> erases;
	for(uint i = 0; i < current_ios.size(); i++) {
		if (current_ios[i].get_bus_wait_time() > 100) {
			overdue_events.push_back(current_ios[i]);
		}
		else if (current_ios[i].get_event_type() == READ_COMMAND) {
			read_commands.push_back(current_ios[i]);
		}
		else if (current_ios[i].get_event_type() == READ_TRANSFER) {
			read_transfers.push_back(current_ios[i]);
		}
		else if (current_ios[i].get_event_type() == WRITE) {
			writes.push_back(current_ios[i]);
		}
		else if (current_ios[i].get_event_type() == ERASE) {
			erases.push_back(current_ios[i]);
		}
	}
	handle_overdue_events(overdue_events);
	execute_next_batch(erases);
	execute_next_batch(read_commands);
	execute_next_batch(read_transfers);
	handle_writes(writes);
}

void IOScheduler::handle_overdue_events(std::vector<Event>& events) {
	for (uint i = 0; i < events.size(); i++) {
		Event event = events[i];
		double time = in_how_long_can_this_event_be_scheduled(event);
		event.incr_bus_wait_time(time);
		event.incr_time_taken(time);
		execute_next(event);
	}
}

// Looks for an idle LUN and schedules writes in it. Works in O(events + LUNs)
/*void IOScheduler::handle_writes(std::vector<Event>& events) {
	if (events.size() == 0) {
		return;
	}
	std::sort(events.begin(), events.end(), bus_wait_time_comparator);
	double start_time = events.back().get_start_time() + events.back().get_bus_wait_time();
	for (uint i = 0; i < SSD_SIZE; i++) {
		double channel_finish_time = ssd.bus.get_channel(i).get_currently_executing_operation_finish_time();
		if (start_time < channel_finish_time) {
			continue;
		}
		for (uint j = 0; j < PACKAGE_SIZE; j++) {
			double die_finish_time = ssd.getPackages()[i].getDies()[j].get_currently_executing_io_finish_time();
			if (start_time >= die_finish_time && events.size() > 0) {
				Address free_page = Block_manager_parallel::instance()->get_free_page(i, j);
				Event write = events.back();
				events.pop_back();
				write.set_address(free_page);
				execute_next(write);
			}
		}
	}
	for (uint i = 0; i < events.size(); i++) {
		events[i].incr_bus_wait_time(1);
		events[i].incr_time_taken(1);
		io_schedule.push_back(events[i]);
	}
}*/

// Looks for an idle LUN and schedules writes in it. Works in O(events * LUNs), but also handles overdue events. Using this for now for simplicity.
void IOScheduler::handle_writes(std::vector<Event>& events) {
	std::sort(events.begin(), events.end(), bus_wait_time_comparator);
	while (events.size() > 0 && Block_manager_parallel::instance()->space_exists_for_next_write()) {
		Address die_with_shortest_queue = get_LUN_with_shortest_queue();
		Address free_page = Block_manager_parallel::instance()->get_next_free_page(die_with_shortest_queue.package, die_with_shortest_queue.die);
		Event event = events.back();
		events.pop_back();
		event.set_address(free_page);
		event.set_noop(false);
		double time = in_how_long_can_this_event_be_scheduled(event);
		if (time == 0) {
			enum status result = execute_next(event);
			Block_manager_parallel::instance()->register_write_outcome(event, result);
		}
		else if (event.get_bus_wait_time() > 50) {
			event.incr_bus_wait_time(time);
			event.incr_time_taken(time);
			enum status result = execute_next(event);
			Block_manager_parallel::instance()->register_write_outcome(event, result);
		}
		else {
			event.incr_bus_wait_time(time);
			event.incr_time_taken(time);
			io_schedule.push_back(event);
		}
	}
	// in case we ran out of free space, put any leftover writes back into the io_schedule for later scheduling
	for (int i = 0; i < events.size(); i++) {
		io_schedule.push_back(events[i]);
	}
}

// Returns the address of the die with the shortest queue that has free space
Address IOScheduler::get_LUN_with_shortest_queue() {
	uint package_id;
	uint die_id;
	double shortest_time = std::numeric_limits<double>::max( );
	for (uint i = 0; i < SSD_SIZE; i++) {
		for (uint j = 0; j < PACKAGE_SIZE; j++) {
			if (Block_manager_parallel::instance()->has_free_pages(i, j)) {
				double channel_finish_time = ssd.bus.get_channel(i).get_currently_executing_operation_finish_time();
				double die_finish_time = ssd.getPackages()[i].getDies()[j].get_currently_executing_io_finish_time();
				double max = std::max(channel_finish_time,die_finish_time);
				if (max < shortest_time) {
					package_id = i;
					die_id = j;
					shortest_time = max;
				}
			}
		}
	}
	return Address(package_id, die_id, 0, 0 , 0, DIE);
}

// executes read_commands, read_transfers and erases
void IOScheduler::execute_next_batch(std::vector<Event>& events) {
	for(uint i = 0; i < events.size(); i++) {
		Event event = events[i];
		assert(event.get_event_type() != WRITE);
		double time = in_how_long_can_this_event_be_scheduled(event);

		if (time <= 0) {
			execute_next(event);
		}
		else {
			event.incr_bus_wait_time(time);
			event.incr_time_taken(time);
			io_schedule.push_back(event);
		}

	}
}


enum status IOScheduler::execute_next(Event& event) {
	enum status result = ssd.controller.issue(event);
	if (result == SUCCESS) {
		int application_io_id = event.get_application_io_id();
		if (dependencies[application_io_id].size() > 0) {
			Event dependent = dependencies[application_io_id].front();
			dependent.set_start_time(event.get_start_time() + event.get_time_taken());
			dependencies[application_io_id].pop();
			io_schedule.push_back(dependent);

		}
		printf("success ");
	} else {
		printf("fail ");
		dependencies.erase(event.get_application_io_id());
	}
	event.print();
	return result;
}

double IOScheduler::in_how_long_can_this_event_be_scheduled(Event& event) {
	double channel_finish_time = ssd.bus.get_channel(event.get_address().package).get_currently_executing_operation_finish_time();
	double die_finish_time = ssd.data[event.get_address().package].get_currently_executing_IO_finish_time_for_spesific_die(event);
	double max = std::max(channel_finish_time, die_finish_time);
	double time = max - event.get_start_time() - event.get_time_taken();
	return time <= 0 ? 0 : time;
}

/*Address IOScheduler::get_LUN_with_shortest_queue() {
	std::vector<double> cumulative_waiting_time_channels(SSD_SIZE, 0);
	std::vector<std::vector<double> > cumulative_waiting_time_dies(SSD_SIZE, std::vector<double>(PACKAGE_SIZE, 0));

	for (uint i = 0; i < io_schedule.size(); i++) {
		uint package_id = io_schedule[i].get_address().package;
		uint die_id = io_schedule[i].get_address().die;
		double expected_duration;
		Event& event = io_schedule[i];
		if (event.get_event_type() == READ_COMMAND) {
			expected_duration = BUS_CTRL_DELAY + PAGE_READ_DELAY;
		}
		else if (event.get_event_type() == READ_TRANSFER) {
			expected_duration = BUS_CTRL_DELAY + BUS_DATA_DELAY;
		}
		else if (event.get_event_type() == ERASE) {
			expected_duration = BUS_CTRL_DELAY + BLOCK_ERASE_DELAY;
		}
		cumulative_waiting_time_channels[package_id] += expected_duration;
		cumulative_waiting_time_dies[package_id][die_id] += expected_duration;
	}

	for (uint i = 0; i < SSD_SIZE; i++) {
		for (uint j = 0; j < PACKAGE_SIZE; j++) {

		}
	}

	uint package_id_with_shortest_queue;
	double shortest_time = std::numeric_limits<double>::max( );

	for (uint i = 0; i < cumulative_waiting_time_channels.size(); i++) {
		if (cumulative_waiting_time_channels[i] < shortest_time) {
			shortest_time = cumulative_waiting_time_channels[i];
			package_id_with_shortest_queue = i;
		}
	}

	shortest_time = std::numeric_limits<double>::max( );
	uint die_id_with_shortest_queue;

	for (uint i = 0; i < cumulative_waiting_time_dies[package_id_with_shortest_queue].size(); i++) {
		double total = cumulative_waiting_time_dies[package_id_with_shortest_queue][i];
		if (total < shortest_time) {
			shortest_time = total;
			die_id_with_shortest_queue = i;
		}
	}

	return Address(package_id_with_shortest_queue, die_id_with_shortest_queue, 0, 0 ,0, DIE);
}*/
