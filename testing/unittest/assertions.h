#pragma once

#include <functional>
#include <iterator>

#include <unittest/exceptions.h>
#include <unittest/util.h>

#define ASSERT_EQUAL_QUIET(X,Y)  unittest::assert_equal_quiet((X),(Y), __FILE__, __LINE__)
#define ASSERT_EQUAL(X,Y)        unittest::assert_equal((X),(Y), __FILE__,  __LINE__)
#define ASSERT_LEQUAL(X,Y)       unittest::assert_lequal((X),(Y), __FILE__,  __LINE__)
#define ASSERT_GEQUAL(X,Y)       unittest::assert_gequal((X),(Y), __FILE__,  __LINE__)
#define ASSERT_ALMOST_EQUAL(X,Y) unittest::assert_almost_equal((X),(Y), __FILE__, __LINE__)
#define KNOWN_FAILURE            { unittest::UnitTestKnownFailure f; f << "[" << __FILE__ ":" << __LINE__ << "]"; throw f;}

#define ASSERT_EQUAL_RANGES(X,Y,Z)  unittest::assert_equal((X),(Y),(Z), __FILE__,  __LINE__)

#define ASSERT_THROWS(X,Y)                                                         \
    {   bool thrown = false; try { X; } catch (Y) { thrown = true; }                  \
        if (!thrown) { unittest::UnitTestFailure f; f << "[" << __FILE__ << ":" << __LINE__ << "] did not throw " << #Y; throw f; } \
    }



namespace unittest
{

static size_t MAX_OUTPUT_LINES = 10;

static double DEFAULT_RELATIVE_TOL = 1e-4;
static double DEFAULT_ABSOLUTE_TOL = 1e-4;

////
// check scalar values
template <typename T1, typename T2>
void assert_equal(const T1& a, const T2& b, 
                  const std::string& filename = "unknown", int lineno = -1)
{
    if(!(a == b)){
        unittest::UnitTestFailure f;
        f << "[" << filename << ":" << lineno << "] ";
        f << "values are not equal: " << a << " " << b;
        f << " [type='" << type_name<T1>() << "']";
        throw f;
    }
}

// sometimes it's not possible to << a type
template <typename T1, typename T2>
void assert_equal_quiet(const T1& a, const T2& b, 
                        const std::string& filename = "unknown", int lineno = -1)
{
    if(!(a == b)){
        unittest::UnitTestFailure f;
        f << "[" << filename << ":" << lineno << "] ";
        f << "values are not equal.";
        f << " [type='" << type_name<T1>() << "']";
        throw f;
    }
}

template <typename T1, typename T2>
void assert_lequal(const T1& a, const T2& b, 
                   const std::string& filename = "unknown", int lineno = -1)
{
    if(!(a <= b)){
        unittest::UnitTestFailure f;
        f << "[" << filename << ":" << lineno << "] ";
        f << a << " is greater than " << b;
        f << " [type='" << type_name<T1>() << "']";
        throw f;
    }
}

template <typename T1, typename T2>
void assert_gequal(const T1& a, const T2& b, 
                   const std::string& filename = "unknown", int lineno = -1)
{
    if(!(a >= b)){
        unittest::UnitTestFailure f;
        f << "[" << filename << ":" << lineno << "] ";
        f << a << " is less than " << b;
        f << " [type='" << type_name<T1>() << "']";
        throw f;
    }
}

// define our own abs() because std::abs() isn't portable for all types for some reason
template<typename T>
  T abs(const T &x)
{
  return x > 0 ? x : -x;
}


inline
bool almost_equal(const double& a, const double& b, const double& a_tol, const double& r_tol)
{
    if(abs(a - b) > r_tol * (abs(a) + abs(b)) + a_tol)
        return false;
    else
        return true;
}

template <typename T1, typename T2>
void assert_almost_equal(const T1& a, const T2& b, 
                         const std::string& filename = "unknown", int lineno = -1,
                         double a_tol = DEFAULT_ABSOLUTE_TOL, double r_tol = DEFAULT_RELATIVE_TOL)

{
    if(!almost_equal(a, b, a_tol, r_tol)){
        unittest::UnitTestFailure f;
        f << "[" << filename << ":" << lineno << "] ";
        f << "values are not approximately equal: " << (double) a << " " << (double) b;
        f << " [type='" << type_name<T1>() << "']";
        throw f;
    }
}

template <typename T>
class almost_equal_to
{
    public:
        double a_tol, r_tol;
        almost_equal_to(double _a_tol = DEFAULT_ABSOLUTE_TOL, double _r_tol = DEFAULT_RELATIVE_TOL) : a_tol(_a_tol), r_tol(_r_tol) {}
        bool operator()(const T& a, const T& b) const {
            return almost_equal(a, b, a_tol, r_tol);
        }
};

////
// check sequences

template <typename ForwardIterator, typename BinaryPredicate>
void assert_equal(ForwardIterator first1, ForwardIterator last1, ForwardIterator first2, const BinaryPredicate& op,
                  const std::string& filename = "unknown", int lineno = -1)
{
    size_t i = 0;
    size_t mismatches = 0;
    
    typedef typename std::iterator_traits<ForwardIterator>::value_type InputType;

    unittest::UnitTestFailure f;
    f << "[" << filename << ":" << lineno << "] ";
    f << "Sequences are not equal [type='" << type_name<InputType>() << "']\n";
    f << "--------------------------------\n";

    while(first1 != last1){
        if(!op(*first1, *first2)){
            mismatches++;
            if(mismatches <= MAX_OUTPUT_LINES)
                f << "  [" << i << "] " << *first1 << "  " << *first2 << "\n";
        }

        first1++;
        first2++;
        i++;
    }


    if (mismatches > 0){
        if(mismatches > MAX_OUTPUT_LINES)
            f << "  (output limit reached)\n";
        f << "--------------------------------\n";
        f << "Sequences differ at " << mismatches << " of " << i << " positions" << "\n";
        throw f;
    }

}

template <typename ForwardIterator>
void assert_equal(ForwardIterator first1, ForwardIterator last1, ForwardIterator first2,
                  const std::string& filename = "unknown", int lineno = -1)
{
    typedef typename std::iterator_traits<ForwardIterator>::value_type InputType;
    assert_equal(first1, last1, first2, std::equal_to<InputType>(), filename, lineno);
}


template <typename ForwardIterator>
void assert_almost_equal(ForwardIterator first1, ForwardIterator last1, ForwardIterator first2, 
                         const std::string& filename = "unknown", int lineno = -1,
                         const double a_tol = DEFAULT_ABSOLUTE_TOL, const double r_tol = DEFAULT_RELATIVE_TOL)
{
    typedef typename std::iterator_traits<ForwardIterator>::value_type InputType;
    assert_equal(first1, last1, first2, almost_equal_to<InputType>(a_tol, r_tol), filename, lineno);
}

}; //end namespace unittest
