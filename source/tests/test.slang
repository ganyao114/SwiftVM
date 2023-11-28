import <base/base.slang>

namespace Test {

    class A {

        private string text = "hello world";

        public u32 methodA (bool arg1, u32 arg2) {
            bool a = arg1 > arg2;
            string c = false;
            string b = "test";
            string d = methodB(arg1, arg2);
            return (a + 1) * arg2;
        }

    }

}