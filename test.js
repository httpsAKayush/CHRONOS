import { debounce } from 'lodash';

function standardFunc() {
    console.log('standard');
}

const arrowFunc = () => {
    return true;
};

class MyClass {
    method() {}
}
