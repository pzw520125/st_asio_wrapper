/*
 * st_asio_wrapper_object_pool.h
 *
 *  Created on: 2013-8-7
 *      Author: youngwolf
 *		email: mail2tao@163.com
 *		QQ: 676218192
 *		Community on QQ: 198941541
 *
 * this class used at both client and server endpoint, and in both TCP and UDP socket
 * this class can only manage objects that inherit from st_socket
 */

#ifndef ST_ASIO_WRAPPER_OBJECT_POOL_H_
#define ST_ASIO_WRAPPER_OBJECT_POOL_H_

#include <boost/atomic.hpp>
#include <boost/unordered_set.hpp>

#include "st_asio_wrapper_timer.h"
#include "st_asio_wrapper_service_pump.h"

#ifndef ST_ASIO_MAX_OBJECT_NUM
#define ST_ASIO_MAX_OBJECT_NUM	4096
#endif

//define ST_ASIO_REUSE_OBJECT macro will enable object pool, all objects in temp_object_can will never be freed, but kept for reusing,
//otherwise, st_object_pool will free objects in temp_object_can automatically and periodically, ST_ASIO_FREE_OBJECT_INTERVAL means the interval, unit is second,
//see temp_object_can at the end of st_object_pool class for more details.
//please note that even if you defined ST_ASIO_REUSE_OBJECT macro, ST_ASIO_FREE_OBJECT_INTERVAL macro is still useful, it will make st_object_pool
//to close (just close, not free, Object must has close function which takes no parameter) objects automatically and periodically for saving SOCKET handles.
#ifndef ST_ASIO_REUSE_OBJECT
	#ifndef ST_ASIO_FREE_OBJECT_INTERVAL
	#define ST_ASIO_FREE_OBJECT_INTERVAL	10 //seconds
	#endif
#endif

//define ST_ASIO_CLEAR_OBJECT_INTERVAL macro to let st_object_pool to invoke clear_obsoleted_object() automatically and periodically
//this feature may affect performance with huge number of objects, so re-write st_server_socket_base::on_recv_error and invoke st_object_pool::del_object()
//is recommended for long-term connection system, but for short-term connection system, you are recommended to open this feature.
//you must define this macro as a value, not just define it, the value means the interval, unit is second
//#define ST_ASIO_CLEAR_OBJECT_INTERVAL		60 //seconds

//after this duration, corresponding objects in temp_object_can can be freed from the heap or reused,
//you must define this macro as a value, not just define it, the value means the duration, unit is second.
//if macro ST_ASIO_ENHANCED_STABILITY been defined, this macro is useless, object's life time is always zero.
#ifndef ST_ASIO_OBSOLETED_OBJECT_LIFE_TIME
#define ST_ASIO_OBSOLETED_OBJECT_LIFE_TIME	5 //seconds
#endif

namespace st_asio_wrapper
{

template<typename Object>
class st_object_pool : public st_service_pump::i_service, public st_timer
{
public:
	typedef boost::shared_ptr<Object> object_type;
	typedef const object_type object_ctype;

protected:
	struct st_object_hasher
	{
	public:
		size_t operator()(object_ctype& object_ptr) const {return (size_t) object_ptr->id();}
		size_t operator()(uint_fast64_t id) const {return (size_t) id;}
	};

	struct st_object_equal
	{
	public:
		bool operator()(object_ctype& left, object_ctype& right) const {return left->id() == right->id();}
		bool operator()(uint_fast64_t id, object_ctype& right) const {return id == right->id();}
	};

public:
	typedef boost::unordered::unordered_set<object_type, st_object_hasher, st_object_equal> container_type;

protected:
	struct temp_object
	{
		object_ctype object_ptr;

#ifdef ST_ASIO_ENHANCED_STABILITY
		temp_object(object_ctype& object_ptr_) : object_ptr(object_ptr_) {assert(object_ptr);}

		bool is_timeout() const {return true;}
		bool is_timeout(time_t now) const {return true;}
#else
		const time_t closed_time;
		temp_object(object_ctype& object_ptr_) : object_ptr(object_ptr_), closed_time(time(nullptr)) {assert(object_ptr);}

		bool is_timeout() const {return is_timeout(time(nullptr));}
		bool is_timeout(time_t now) const {return closed_time <= now - ST_ASIO_OBSOLETED_OBJECT_LIFE_TIME;}
#endif
	};

protected:
	static const unsigned char TIMER_BEGIN = st_timer::TIMER_END;
	static const unsigned char TIMER_FREE_SOCKET = TIMER_BEGIN;
	static const unsigned char TIMER_CLEAR_SOCKET = TIMER_BEGIN + 1;
	static const unsigned char TIMER_END = TIMER_BEGIN + 10;

	st_object_pool(st_service_pump& service_pump_) : i_service(service_pump_), st_timer(service_pump_), cur_id(-1), max_size_(ST_ASIO_MAX_OBJECT_NUM) {}

	void start()
	{
#ifdef ST_ASIO_FREE_OBJECT_INTERVAL
		set_timer(TIMER_FREE_SOCKET, 1000 * ST_ASIO_FREE_OBJECT_INTERVAL, [this](unsigned char id)->bool {assert(TIMER_FREE_SOCKET == id); ST_THIS free_object(); return true;});
#endif
#ifdef ST_ASIO_CLEAR_OBJECT_INTERVAL
		set_timer(TIMER_CLEAR_SOCKET, 1000 * ST_ASIO_CLEAR_OBJECT_INTERVAL, [this](unsigned char id)->bool {assert(TIMER_CLEAR_SOCKET == id); ST_THIS clear_obsoleted_object(); return true;});
#endif
	}

	void stop() {stop_all_timer();}

	bool add_object(object_ctype& object_ptr)
	{
		assert(object_ptr);

		boost::unique_lock<boost::shared_mutex> lock(object_can_mutex);
		return object_can.size() < max_size_ ? object_can.insert(object_ptr).second : false;
	}

	//only add object_ptr to temp_object_can when it's in object_can, this can avoid duplicated items in temp_object_can, because temp_object_can is a list, there's no way to check the existence
	//of an item in a list efficiently.
	bool del_object(object_ctype& object_ptr)
	{
		assert(object_ptr);

		boost::unique_lock<boost::shared_mutex> lock(object_can_mutex);
		auto exist = object_can.erase(object_ptr) > 0;
		lock.unlock();

		if (exist)
		{
			boost::unique_lock<boost::shared_mutex> lock(temp_object_can_mutex);
			temp_object_can.push_back(object_ptr);
		}

		return exist;
	}

	virtual void post_create(object_ctype& object_ptr) {if (object_ptr) object_ptr->id(++cur_id);}

#ifdef ST_ASIO_REUSE_OBJECT
	object_type reuse_object()
	{
		boost::unique_lock<boost::shared_mutex> lock(temp_object_can_mutex);
		//objects are order by time, so we don't have to go through all items in temp_object_can
		for (auto iter = std::begin(temp_object_can); iter != std::end(temp_object_can) && iter->is_timeout(); ++iter)
			if (iter->object_ptr.unique() && iter->object_ptr->obsoleted())
			{
				auto object_ptr(std::move(iter->object_ptr));
				temp_object_can.erase(iter);
				lock.unlock();

				object_ptr->reset();
				return object_ptr;
			}

		return object_type();
	}

public:
	object_type create_object()
	{
		auto object_ptr = reuse_object();
		if (!object_ptr)
			object_ptr = boost::make_shared<Object>(service_pump);

		return post_create(object_ptr), object_ptr;
	}

	template<typename Arg>
	object_type create_object(Arg& arg)
	{
		auto object_ptr = reuse_object();
		if (!object_ptr)
			object_ptr = boost::make_shared<Object>(arg);

		return post_create(object_ptr), object_ptr;
	}
#else
public:
	object_type create_object() {auto object_ptr = boost::make_shared<Object>(service_pump); return post_create(object_ptr), object_ptr;}

	template<typename Arg>
	object_type create_object(Arg& arg) {auto object_ptr = boost::make_shared<Object>(arg); return post_create(object_ptr), object_ptr;}
#endif

	//to configure unordered_set(for example, set factor or reserved size), not locked the mutex, so must be called before service_pump starting up.
	container_type& container() {return object_can;}

	size_t max_size() const {return max_size_;}
	void max_size(size_t _max_size) {max_size_ = _max_size;}

	size_t size()
	{
		boost::shared_lock<boost::shared_mutex> lock(object_can_mutex);
		return object_can.size();
	}

	size_t closed_object_size()
	{
		boost::shared_lock<boost::shared_mutex> lock(temp_object_can_mutex);
		return temp_object_can.size();
	}

	//this method has linear complexity, please note.
	object_type at(size_t index)
	{
		boost::shared_lock<boost::shared_mutex> lock(object_can_mutex);
		assert(index < object_can.size());
		return index < object_can.size() ? *(std::next(std::begin(object_can), index)) : object_type();
	}

	//this method has linear complexity, please note.
	object_type closed_object_at(size_t index)
	{
		boost::shared_lock<boost::shared_mutex> lock(temp_object_can_mutex);
		assert(index < temp_object_can.size());
		return index < temp_object_can.size() ? std::next(std::begin(temp_object_can), index)->object_ptr : object_type();
	}

	object_type find(uint_fast64_t id)
	{
		boost::shared_lock<boost::shared_mutex> lock(object_can_mutex);
		auto iter = object_can.find(id, st_object_hasher(), st_object_equal());
		return iter != std::end(object_can) ? *iter : object_type();
	}

	object_type closed_object_find(uint_fast64_t id)
	{
		boost::shared_lock<boost::shared_mutex> lock(temp_object_can_mutex);
		for (auto iter = std::begin(temp_object_can); iter != std::end(temp_object_can); ++iter)
			if (id == iter->object_ptr->id())
				return iter->object_ptr;
		return object_type();
	}

	object_type closed_object_pop(uint_fast64_t id)
	{
		boost::shared_lock<boost::shared_mutex> lock(temp_object_can_mutex);
		for (auto iter = std::begin(temp_object_can); iter != std::end(temp_object_can); ++iter)
			if (id == iter->object_ptr->id())
			{
				auto object_ptr = iter->object_ptr;
				temp_object_can.erase(iter);
				return object_ptr;
			}
		return object_type();
	}

	void list_all_object() {do_something_to_all([](object_ctype& item) {item->show_info("", ""); });}

	//Kick out obsoleted objects
	//Consider the following assumptions:
	//1.You didn't invoke del_object in on_recv_error or other places.
	//2.For some reason(I haven't met yet), on_recv_error not been invoked
	//st_object_pool will automatically invoke this function if ST_ASIO_CLEAR_OBJECT_INTERVAL been defined
	size_t clear_obsoleted_object()
	{
		container_type objects;

		boost::unique_lock<boost::shared_mutex> lock(object_can_mutex);
		for (auto iter = std::begin(object_can); iter != std::end(object_can);)
			if ((*iter).unique() && (*iter)->obsoleted())
			{
				(*iter)->show_info("object:", "is obsoleted, kick it out, it will be freed or reused in the future.");
#ifdef ST_ASIO_ENHANCED_STABILITY
				(*iter)->close();
#endif
				objects.insert(*iter);
				iter = object_can.erase(iter);
			}
			else
				++iter;
		lock.unlock();

		auto size = objects.size();
		if (0 != size)
		{
			unified_out::warning_out(ST_ASIO_SF " object(s) been kicked out!", size);

			boost::unique_lock<boost::shared_mutex> lock(temp_object_can_mutex);
			temp_object_can.insert(std::end(temp_object_can), std::begin(objects), std::end(objects));
		}

		return size;
	}

	//free or close a specific number of objects
	//if you used object pool(define ST_ASIO_REUSE_OBJECT), you can manually call this function to free some objects after the object pool(get_closed_object_size())
	// goes big enough for memory saving(because the objects in temp_object_can are waiting for reusing and will never be freed),
	// you can also define ST_ASIO_FREE_OBJECT_INTERVAL to let st_object_pool to call this function automatically and periodically, but objects will only be closed.
	//if you don't used object pool, st_object_pool will invoke this function automatically and periodically, so you don't need to invoke this function exactly
	//return affected object number, if just_close equal to true, then closed objects will be treated as unaffected.
#ifdef ST_ASIO_REUSE_OBJECT
	size_t free_object(size_t num = -1, bool just_close = true)
#else
	size_t free_object(size_t num = -1, bool just_close = false)
#endif
	{
		size_t num_affected = 0;
		boost::unique_lock<boost::shared_mutex> lock(temp_object_can_mutex);
		//objects are order by time, so we don't have to go through all items in temp_object_can
		for (auto iter = std::begin(temp_object_can); num > 0 && iter != std::end(temp_object_can) && iter->is_timeout();)
			if (iter->object_ptr.unique() && iter->object_ptr->obsoleted())
			{
				--num;
				if (just_close)
				{
					if (iter->object_ptr->close())
					{
						unified_out::info_out("closed an object.");
						++num_affected;
					}
					++iter;
				}
				else
				{
					unified_out::info_out("freed an object.");
					++num_affected;
					iter = temp_object_can.erase(iter);
				}
			}
			else
				++iter;

		if (num_affected > 0)
			unified_out::warning_out(ST_ASIO_SF " object(s) been %s!", num_affected, just_close ? "closed" : "freed");

		return num_affected;
	}

	DO_SOMETHING_TO_ALL_MUTEX(object_can, object_can_mutex)
	DO_SOMETHING_TO_ONE_MUTEX(object_can, object_can_mutex)

protected:
	boost::atomic_uint_fast64_t cur_id;

	container_type object_can;
	boost::shared_mutex object_can_mutex;
	size_t max_size_;

	//because all objects are dynamic created and stored in object_can, maybe when receiving error occur
	//(you are recommended to delete the object from object_can, for example via st_server_base::del_client), some other asynchronous calls are still queued in boost::asio::io_service,
	//and will be dequeued in the future, we must guarantee these objects not be freed from the heap or reused, so we move these objects from object_can to temp_object_can,
	//and free them from the heap or reuse them in the near future, see ST_ASIO_OBSOLETED_OBJECT_LIFE_TIME macro for more details.
	//if ST_ASIO_CLEAR_OBJECT_INTERVAL been defined, clear_obsoleted_object() will be invoked automatically and periodically to move all closed objects into temp_object_can.
	boost::container::list<temp_object> temp_object_can;
	boost::shared_mutex temp_object_can_mutex;
};

} //namespace

#endif /* ST_ASIO_WRAPPER_OBJECT_POOL_H_ */
