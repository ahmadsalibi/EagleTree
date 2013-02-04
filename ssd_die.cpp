/* Copyright 2009, 2010 Brendan Tauras */

/* ssd_die.cpp is part of FlashSim. */

/* FlashSim is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version. */

/* FlashSim is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details. */

/* You should have received a copy of the GNU General Public License
 * along with FlashSim.  If not, see <http://www.gnu.org/licenses/>. */

/****************************************************************************/

/* Die class
 * Brendan Tauras 2009-11-03
 *
 * The die is the data storage hardware unit that contains planes and is a flash
 * chip.  Dies maintain wear statistics for the FTL. */

#include <new>
#include <assert.h>
#include <stdio.h>
#include "ssd.h"

using namespace ssd;

Die::Die(const Package &parent, Channel &channel, uint die_size, long physical_address):
	size(die_size),

	/* use a const pointer (Plane * const data) to use as an array
	 * but like a reference, we cannot reseat the pointer */
	data((Plane *) malloc(size * sizeof(Plane))),
	parent(parent),
	channel(channel),

	/* assume all Planes are same so first one can start as least worn */
	least_worn(0),

	/* set erases remaining to BLOCK_ERASES to match Block constructor args 
	 * in Plane class
	 * this is the cheap implementation but can change to pass through classes */
	erases_remaining(BLOCK_ERASES),

	/* assume hardware created at time 0 and had an implied free erasure */
	last_erase_time(0.0),
	currently_executing_io_finish_time(0.0),
	last_read_io(-1)
{
	uint i;

	//if(channel.connect() == FAILURE)
	//	fprintf(stderr, "Die error: %s: constructor unable to connect to Bus Channel\n", __func__);

	/* new cannot initialize an array with constructor args so
	 * 	malloc the array
	 * 	then use placement new to call the constructor for each element
	 * chose an array over container class so we don't have to rely on anything
	 * 	i.e. STL's std::vector */
	/* array allocated in initializer list:
	 * data = (Plane *) malloc(size * sizeof(Plane)); */
	if(data == NULL){
		fprintf(stderr, "Die error: %s: constructor unable to allocate Plane data\n", __func__);
		exit(MEM_ERR);
	}

	for(i = 0; i < size; i++)
		(void) new (&data[i]) Plane(*this, PLANE_SIZE, PLANE_REG_READ_DELAY, PLANE_REG_WRITE_DELAY, physical_address+(PLANE_SIZE*BLOCK_SIZE*i));

	return;
}

Die::~Die(void)
{
	assert(data != NULL);
	uint i;
	/* call destructor for each Block array element
	 * since we used malloc and placement new */
	for(i = 0; i < size; i++)
		data[i].~Plane();
	free(data);
	//(void) channel.disconnect();
	return;
}

enum status Die::read(Event &event)
{
	assert(data != NULL);
	assert(event.get_address().plane < size && event.get_address().valid > DIE);
	assert(currently_executing_io_finish_time <= event.get_current_time());
	if (event.get_event_type() == READ_COMMAND) {
		last_read_io = event.get_application_io_id();
	}

	enum status result = data[event.get_address().plane].read(event);
	currently_executing_io_finish_time = event.get_current_time();
	return result;
}

enum status Die::write(Event &event)
{
	assert(data != NULL);
	assert(event.get_address().plane < size && event.get_address().valid > DIE);
	assert(currently_executing_io_finish_time <= event.get_current_time());
	//last_read_io = event.get_application_io_id();
	enum status result = data[event.get_address().plane].write(event);
	currently_executing_io_finish_time = event.get_current_time();
	return result;
}

/* if no errors
 * 	updates last_erase_time if later time
 * 	updates erases_remaining if smaller value
 * returns 1 for success, 0 for failure */
enum status Die::erase(Event &event)
{
	assert(data != NULL);
	assert(event.get_address().plane < size && event.get_address().valid > DIE);

	assert(currently_executing_io_finish_time <= event.get_current_time());

	//last_read_io = event.get_application_io_id();
	enum status status = data[event.get_address().plane].erase(event);
	currently_executing_io_finish_time = event.get_current_time();
	/* update values if no errors */
	if(status == SUCCESS)
		update_wear_stats(event.get_address());
	return status;
}

double Die::get_currently_executing_io_finish_time() {
	return currently_executing_io_finish_time;
}

const Package &Die::get_parent(void) const
{
	return parent;
}

/* if given a valid Block address, call the Block's method
 * else return local value */
double Die::get_last_erase_time(const Address &address) const
{
	assert(data != NULL);
	if(address.valid > DIE && address.plane < size)
		return data[address.plane].get_last_erase_time(address);
	else
		return last_erase_time;
}

/* if given a valid Plane address, call the Plane's method
 * else return local value */
ulong Die::get_erases_remaining(const Address &address) const
{
	assert(data != NULL);
	if(address.valid > DIE && address.plane < size)
		return data[address.plane].get_erases_remaining(address);
	else
		return erases_remaining;
}



/* Plane with the most erases remaining is the least worn */
void Die::update_wear_stats(const Address &address)
{
	assert(data != NULL);
	uint i;
	uint max_index = 0;
	ulong max = data[0].get_erases_remaining(address);
	for(i = 1; i < size; i++)
		if(data[i].get_erases_remaining(address) > max)
			max_index = i;
	least_worn = max_index;
	erases_remaining = max;
	last_erase_time = data[max_index].get_last_erase_time(address);
	return;
}

enum page_state Die::get_state(const Address &address) const
{  
	assert(data != NULL && address.plane < size && address.valid >= DIE);
	return data[address.plane].get_state(address);
}

enum block_state Die::get_block_state(const Address &address) const
{
	assert(data != NULL && address.plane < size && address.valid >= DIE);
	return data[address.plane].get_block_state(address);
}

void Die::get_free_page(Address &address) const
{
	assert(address.plane < size && address.valid >= PLANE);
	data[address.plane].get_free_page(address);
	return;
}

uint Die::get_num_free(const Address &address) const
{
	assert(address.valid >= PLANE);
	return data[address.plane].get_num_free(address);
}   

uint Die::get_num_valid(const Address &address) const
{
	assert(address.valid >= PLANE);
	return data[address.plane].get_num_valid(address);
}   

uint Die::get_num_invalid(const Address & address) const
{
	assert(address.valid >= PLANE);
	return data[address.plane].get_num_invalid(address);
}

Block *Die::get_block_pointer(const Address & address)
{
	assert(address.valid >= PLANE);
	return data[address.plane].get_block_pointer(address);
}

// Inlined for speed
/*Plane *Die::getPlanes() {
	return data;
}*/

int Die::get_last_read_application_io() {
	return last_read_io;
}

bool Die::register_is_busy() {
	return last_read_io != -1;
}

void Die::clear_register() {
	last_read_io = -1;
}
